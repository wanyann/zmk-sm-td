/* Copyright 2025 Stanislav Markin (https://github.com/stasmarkin)
 * Copyright 2026 ZMK Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define DT_DRV_COMPAT zmk_behavior_sm_td

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sm_td, CONFIG_ZMK_LOG_LEVEL);

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <zmk-sm-td/sm_td.h>

#define IS_SMTD_ACTIVE                                                                             \
    (IS_ENABLED(CONFIG_ZMK_SPLIT) ? (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) : (1))

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && IS_SMTD_ACTIVE

struct behavior_sm_td_config {
    struct smtd_config core_config;
    const char *hold_dev_name;
    const char *tap_dev_name;
};

struct behavior_sm_td_data {
    struct smtd_runtime runtime;
    /* Parameter captured from the keymap binding on press, forwarded to the
     * sub-behaviors when the hold/tap action is resolved. */
    uint32_t hold_param;
    uint32_t tap_param;
    uint8_t binding_layer;
    /* Keymap-provided override callback (v1: always NULL). */
    smtd_resolution (*on_action)(struct behavior_sm_td_data *data, smtd_action action,
                                 uint8_t tap_count);
};

/* ------------------------------------------------------------------ *
 *                GLOBAL INSTANCE REGISTRY + CAPTURE                   *
 * ------------------------------------------------------------------ */

#define MAX_INSTANCES 4

static struct behavior_sm_td_data *instances[MAX_INSTANCES];
static uint8_t instance_count = 0;

#define CAPTURED_MAX SMTD_CAPTURED_EVENTS_SIZE

/* Global FIFO of captured position_state_changed events, re-raised once no
 * instance is undecided anymore. Stored globally (not per-instance) so a
 * single physical key event is released exactly once. */
struct captured_event {
    struct zmk_position_state_changed_event data;
};

static struct captured_event captured[CAPTURED_MAX];
static uint8_t captured_head = 0;
static uint8_t captured_count = 0;

static bool releasing_captured = false;
static bool emitting_binding = false;

static inline void emit_guard(void) { emitting_binding = true; }
static inline void emit_unguard(void) { emitting_binding = false; }

static void smtd_register_instance(struct behavior_sm_td_data *data) {
    if (instance_count >= MAX_INSTANCES) {
        return;
    }
    for (uint8_t i = 0; i < instance_count; i++) {
        if (instances[i] == data) {
            return;
        }
    }
    instances[instance_count++] = data;
}

static bool smtd_any_undecided(void) {
    for (uint8_t i = 0; i < instance_count; i++) {
        if (smtd_has_undecided(&instances[i]->runtime)) {
            return true;
        }
    }
    return false;
}

static bool smtd_position_is_ours(uint32_t position) {
    for (uint8_t i = 0; i < instance_count; i++) {
        if (smtd_owns_position(&instances[i]->runtime, position)) {
            return true;
        }
    }
    return false;
}

/* Is `position` bound to one of our sm_td behaviors on the currently active
 * layer? Used so the own sm_td key bubbles (its binding handlers drive the
 * state machine) instead of being captured as an "other" key. */
static struct behavior_sm_td_data *smtd_find_for_position(uint32_t position) {
    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t layer = zmk_keymap_layer_index_to_id(layer_index);
    const struct zmk_behavior_binding *binding =
        zmk_keymap_get_layer_binding_at_idx(layer, position);
    if (binding == NULL || binding->behavior_dev == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < instance_count; i++) {
        struct behavior_sm_td_data *data = instances[i];
        if (strcmp(binding->behavior_dev, data->runtime.device->name) == 0) {
            return data;
        }
    }
    return NULL;
}

static bool smtd_capture_store(const struct zmk_position_state_changed *ev) {
    if (captured_count >= CAPTURED_MAX) {
        return false;
    }
    uint8_t tail = (captured_head + captured_count) % CAPTURED_MAX;
    captured[tail].data = copy_raised_zmk_position_state_changed(ev);
    captured_count++;
    return true;
}

static void smtd_capture_release(void) {
    if (captured_count == 0 || releasing_captured) {
        return;
    }
    releasing_captured = true;
    while (captured_count > 0) {
        struct captured_event ev = captured[captured_head];
        captured_head = (captured_head + 1) % CAPTURED_MAX;
        captured_count--;
        /* Re-raise at our own listener: any new sm_td work will re-capture
         * where appropriate, otherwise the event flows on to the keymap. */
        ZMK_EVENT_RAISE_AT(ev.data, sm_td_listener);
    }
    releasing_captured = false;
}

/* ------------------------------------------------------------------ *
 *                      SUB-BEHAVIOR EMISSION                          *
 * ------------------------------------------------------------------ */

static int smtd_invoke(struct behavior_sm_td_data *data, uint8_t which, uint32_t position,
                       bool pressed, int64_t timestamp) {
    const struct behavior_sm_td_config *cfg = data->runtime.device->config;
    const char *dev_name = (which == 0) ? cfg->hold_dev_name : cfg->tap_dev_name;
    uint32_t param = (which == 0) ? data->hold_param : data->tap_param;

    struct zmk_behavior_binding bb = {
        .behavior_dev = dev_name,
        .param1 = param,
        .param2 = 0,
    };
    struct zmk_behavior_binding_event event = {
        .position = position,
        .layer = data->binding_layer,
        .timestamp = timestamp,
    };

    emit_guard();
    int ret = zmk_behavior_invoke_binding(&bb, event, pressed);
    emit_unguard();
    return ret;
}

/* ------------------------------------------------------------------ *
 *                    CORE OUT-CALL IMPLEMENTATIONS                    *
 * ------------------------------------------------------------------ */

smtd_resolution smtd_driver_on_action(smtd_runtime *rt, uint32_t position, smtd_action action,
                                      uint8_t tap_count) {
    struct behavior_sm_td_data *data = CONTAINER_OF(rt, struct behavior_sm_td_data, runtime);

    if (data->on_action != NULL) {
        return data->on_action(data, action, tap_count);
    }

    switch (action) {
    case SMTD_ACTION_TOUCH:
        /* Nothing is emitted yet; stay undecided. */
        return SMTD_RESOLUTION_UNCERTAIN;
    case SMTD_ACTION_TAP:
        smtd_invoke(data, 1, position, true, k_uptime_get());
        smtd_invoke(data, 1, position, false, k_uptime_get());
        return SMTD_RESOLUTION_DETERMINED;
    case SMTD_ACTION_HOLD:
        smtd_invoke(data, 0, position, true, k_uptime_get());
        return SMTD_RESOLUTION_DETERMINED;
    case SMTD_ACTION_RELEASE:
        smtd_invoke(data, 0, position, false, k_uptime_get());
        return SMTD_RESOLUTION_DETERMINED;
    }
    return SMTD_RESOLUTION_UNHANDLED;
}

void smtd_driver_emit_key(smtd_runtime *rt, uint32_t position, bool pressed, int64_t timestamp) {
    struct behavior_sm_td_data *data = CONTAINER_OF(rt, struct behavior_sm_td_data, runtime);
    smtd_invoke(data, 1, position, pressed, timestamp);
}

void smtd_driver_after_resolve(smtd_runtime *rt) {
    (void)rt;
    if (!smtd_any_undecided()) {
        smtd_capture_release();
    }
}

/* ------------------------------------------------------------------ *
 *                     BINDING HANDLERS                               *
 * ------------------------------------------------------------------ */

static int on_sm_td_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_sm_td_data *data = dev->data;

    data->hold_param = binding->param1;
    data->tap_param = binding->param2;
    data->binding_layer = event.layer;

    smtd_process_event(&data->runtime, event.position, event.timestamp, event.timestamp, true);
    smtd_driver_after_resolve(&data->runtime);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sm_td_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_sm_td_data *data = dev->data;

    smtd_process_event(&data->runtime, event.position, event.timestamp, event.timestamp, false);
    smtd_driver_after_resolve(&data->runtime);
    return ZMK_BEHAVIOR_OPAQUE;
}

/* ------------------------------------------------------------------ *
 *                        LISTENER                                     *
 * ------------------------------------------------------------------ */

static int on_position_state_changed(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Never re-process events emitted by our own sub-behaviors. */
    if (emitting_binding || releasing_captured) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Own sm_td key: the binding handlers drive the state machine. Let it
     * bubble to the keymap so binding_pressed/released fire. */
    if (smtd_find_for_position(ev->position) != NULL || smtd_position_is_ours(ev->position)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* "Other" key. Capture it (and feed the state machines) while any
     * instance is still undecided, otherwise let it flow through normally. */
    if (!smtd_any_undecided()) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (uint8_t i = 0; i < instance_count; i++) {
        /* Only instances with pending state need to track this "other" key,
         * which also avoids exhausting every instance's state pool on a roll. */
        if (!smtd_has_undecided(&instances[i]->runtime)) {
            continue;
        }
        smtd_process_event(&instances[i]->runtime, ev->position, ev->timestamp, ev->timestamp,
                           ev->state);
    }
    smtd_driver_after_resolve(NULL);

    if (!smtd_capture_store(ev)) {
        /* Buffer full: dropping the key is worse than resolving it immediately,
         * so let the event continue to the keymap instead of capturing it. */
        LOG_ERR("sm_td capture buffer full, bubbling event");
        return ZMK_EV_EVENT_BUBBLE;
    }
    return ZMK_EV_EVENT_CAPTURED;
}

ZMK_LISTENER(sm_td_listener, on_position_state_changed);
ZMK_SUBSCRIPTION(sm_td_listener, zmk_position_state_changed);

/* ------------------------------------------------------------------ *
 *              LISTENER ORDERING DEBUG (temporary)                    *
 * ------------------------------------------------------------------ */

extern const struct zmk_listener zmk_listener_keymap;
extern struct zmk_event_subscription __event_subscriptions_start[];
extern struct zmk_event_subscription __event_subscriptions_end[];

static void smtd_dump_subscription_order(void) {
    int sm_idx = -1;
    int km_idx = -1;
    long i = 0;
    for (struct zmk_event_subscription *sub = __event_subscriptions_start;
         sub != __event_subscriptions_end; sub++, i++) {
        if (sub->event_type != &zmk_event_zmk_position_state_changed) {
            continue;
        }
        if (sub->listener == &zmk_listener_sm_td) {
            sm_idx = (int)i;
        } else if (sub->listener == &zmk_listener_keymap) {
            km_idx = (int)i;
        }
    }
    LOG_INF("SM_TD listener idx=%d, keymap idx=%d -> sm_td runs %s keymap", sm_idx, km_idx,
            (sm_idx >= 0 && km_idx >= 0 && sm_idx < km_idx) ? "BEFORE" : "AFTER");
}

SYS_INIT(smtd_dump_subscription_order, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

/* ------------------------------------------------------------------ *
 *                        INIT & INSTANCES                            *
 * ------------------------------------------------------------------ */

static int behavior_sm_td_init(const struct device *dev) {
    struct behavior_sm_td_config *cfg = (struct behavior_sm_td_config *)dev->config;
    struct behavior_sm_td_data *data = dev->data;

    memset(&data->runtime, 0, sizeof(data->runtime));
    data->runtime.config = &cfg->core_config;
    data->runtime.device = dev;
    data->hold_param = 0;
    data->tap_param = 0;
    data->binding_layer = 0;
    data->on_action = NULL;

    smtd_reset_runtime(&data->runtime);
    smtd_register_instance(data);
    return 0;
}

static const struct behavior_driver_api behavior_sm_td_driver_api = {
    .binding_pressed = on_sm_td_binding_pressed,
    .binding_released = on_sm_td_binding_released,
};

#define SM_TD_INST(n)                                                                              \
    static struct behavior_sm_td_data behavior_sm_td_data_##n = {0};                              \
    static struct behavior_sm_td_config behavior_sm_td_config_##n = {                             \
        .core_config =                                                                             \
            {                                                                                      \
                .tap_term_ms = DT_INST_PROP(n, tap_term_ms),                                       \
                .sequence_term_ms = DT_INST_PROP(n, sequence_term_ms),                             \
                .release_term_ms = DT_INST_PROP(n, release_term_ms),                               \
                .release_percent = DT_INST_PROP(n, release_percent),                               \
                .aggregate_taps = DT_INST_PROP(n, aggregate_taps),                                 \
            },                                                                                     \
        .hold_dev_name = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),                   \
        .tap_dev_name = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 1)),                    \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_sm_td_init, NULL, &behavior_sm_td_data_##n,                \
                            &behavior_sm_td_config_##n, POST_KERNEL,                               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_sm_td_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SM_TD_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && IS_SMTD_ACTIVE */