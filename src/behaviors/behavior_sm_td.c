/* Copyright 2026 Stanislav Markin (https://github.com/stasmarkin)
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

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_sm_td_config {
    struct smtd_config core_config;
    const struct device *hold_dev;
    const struct device *tap_dev;
};

struct behavior_sm_td_data {
    struct smtd_runtime runtime;
    /* The parameters supplied by the keymap binding, stored on press so the
     * timeout/action resolution can forward them to the sub-behavior. */
    uint32_t hold_param;
    uint32_t tap_param;
    uint8_t binding_layer;
    /* If non-NULL, a keymap-provided override callback. */
    smtd_resolution (*on_action)(struct behavior_sm_td_data *data, smtd_action action, uint8_t tap_count);
};

/* ------------------------------------------------------------------ *
 *                GLOBAL INSTANCE REGISTRY + LISTENER                  *
 * ------------------------------------------------------------------ */

static struct behavior_sm_td_data *registered_data[SMTD_POOL_SIZE];
static uint8_t registered_count = 0;

static void smtd_register_instance(struct behavior_sm_td_data *data) {
    for (uint8_t i = 0; i < registered_count; i++) {
        if (registered_data[i] == data) {
            return;
        }
    }
    if (registered_count < SMTD_POOL_SIZE) {
        registered_data[registered_count++] = data;
    }
}

/* Invoke one of the two declared sub-behaviors (hold=0, tap=1) for the given
 * position, using the params captured at keymap-binding time. */
static int smtd_invoke(const struct device *dev, uint8_t which, struct behavior_sm_td_data *data,
                       uint32_t position, bool pressed, int64_t timestamp) {
    const struct behavior_sm_td_config *cfg = dev->config;
    const struct device *target = (which == 0) ? cfg->hold_dev : cfg->tap_dev;
    uint32_t param = (which == 0) ? data->hold_param : data->tap_param;

    struct zmk_behavior_binding bb = {
        .behavior_dev = target->name,
        .param1 = param,
        .param2 = 0,
    };
    struct zmk_behavior_binding_event event = {
        .position = position,
        .layer = data->binding_layer,
        .timestamp = timestamp,
    };
    return zmk_behavior_invoke_binding(&bb, event, pressed);
}

/* Core out-call: resolve an action for a position. */
smtd_resolution smtd_driver_on_action(smtd_runtime *rt, uint32_t position, smtd_action action, uint8_t tap_count);

/* Core out-call: emit a raw key for a position (used as fallback). */
void smtd_driver_emit_key(smtd_runtime *rt, uint32_t position, bool pressed, int64_t timestamp);

smtd_resolution smtd_driver_on_action(smtd_runtime *rt, uint32_t position, smtd_action action, uint8_t tap_count) {
    for (uint8_t i = 0; i < registered_count; i++) {
        struct behavior_sm_td_data *data = registered_data[i];
        if (&data->runtime != rt) {
            continue;
        }
        const struct device *dev = rt->device;

        if (data->on_action != NULL) {
            return data->on_action(data, action, tap_count);
        }

        switch (action) {
        case SMTD_ACTION_TOUCH:
            return SMTD_RESOLUTION_UNCERTAIN;
        case SMTD_ACTION_TAP:
            smtd_invoke(dev, 1, data, position, true, k_uptime_get());
            smtd_invoke(dev, 1, data, position, false, k_uptime_get());
            return SMTD_RESOLUTION_DETERMINED;
        case SMTD_ACTION_HOLD:
            smtd_invoke(dev, 0, data, position, true, k_uptime_get());
            return SMTD_RESOLUTION_DETERMINED;
        case SMTD_ACTION_RELEASE:
            smtd_invoke(dev, 0, data, position, false, k_uptime_get());
            return SMTD_RESOLUTION_DETERMINED;
        }
    }
    return SMTD_RESOLUTION_UNHANDLED;
}

void smtd_driver_emit_key(smtd_runtime *rt, uint32_t position, bool pressed, int64_t timestamp) {
    for (uint8_t i = 0; i < registered_count; i++) {
        struct behavior_sm_td_data *data = registered_data[i];
        if (&data->runtime != rt) {
            continue;
        }
        const struct device *dev = rt->device;
        /* Emit the TAP binding as a real key, mirroring the pipeline. */
        smtd_invoke(dev, 1, data, position, pressed, timestamp);
        return;
    }
}

/* Listener: forward every physical-key move into each instance's core so the
 * release-timing logic can observe concurrently-held keys. */
static int on_position_state_changed(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_HANDLED;
    }
    for (uint8_t i = 0; i < registered_count; i++) {
        smtd_process_event(&registered_data[i]->runtime, ev->position, ev->timestamp, ev->timestamp, ev->state);
    }
    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(sm_td_listener, on_position_state_changed);
ZMK_SUBSCRIPTION(sm_td_listener, position_state_changed);

/* ------------------------------------------------------------------ *
 *                     BINDING HANDLERS                               *
 * ------------------------------------------------------------------ */

static int on_sm_td_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_sm_td_data *data = dev->data;

    /* The keymap provided the two params: hold (param1) and tap (param2). */
    data->hold_param = binding->param1;
    data->tap_param = binding->param2;
    data->binding_layer = event.layer;

    smtd_process_event(&data->runtime, event.position, event.timestamp, event.timestamp, true);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sm_td_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_sm_td_data *data = dev->data;

    smtd_process_event(&data->runtime, event.position, event.timestamp, event.timestamp, false);
    return ZMK_BEHAVIOR_OPAQUE;
}

/* ------------------------------------------------------------------ *
 *                        INIT & INSTANCES                            *
 * ------------------------------------------------------------------ */

static int behavior_sm_td_init(const struct device *dev) {
    struct behavior_sm_td_config *cfg = (struct behavior_sm_td_config *)dev->config;
    struct behavior_sm_td_data *data = dev->data;

    memset(&data->runtime, 0, sizeof(data->runtime));
    data->runtime.config = &cfg->core_config;
    data->runtime.device = dev;

    smtd_reset_runtime(&data->runtime);
    smtd_register_instance(data);
    return 0;
}

static const struct behavior_driver_api behavior_sm_td_driver_api = {
    .binding_pressed = on_sm_td_binding_pressed,
    .binding_released = on_sm_td_binding_released,
};

#define SM_TD_INST(n)                                                                                              \
    static struct behavior_sm_td_data behavior_sm_td_data_##n = {0};                                              \
    static struct behavior_sm_td_config behavior_sm_td_config_##n = {                                             \
        .core_config =                                                                                            \
            {                                                                                                     \
                .tap_term_ms = DT_INST_PROP(n, tap_term_ms),                                                      \
                .sequence_term_ms = DT_INST_PROP(n, sequence_term_ms),                                            \
                .release_term_ms = DT_INST_PROP(n, release_term_ms),                                              \
                .release_percent = DT_INST_PROP(n, release_percent),                                              \
                .aggregate_taps = DT_INST_PROP(n, aggregate_taps),                                                \
                .retro_tap = DT_INST_PROP(n, retro_tap),                                                          \
            },                                                                                                    \
        .hold_dev = DEVICE_DT_GET(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),                                        \
        .tap_dev = DEVICE_DT_GET(DT_INST_PHANDLE_BY_IDX(n, bindings, 1)),                                         \
    };                                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_sm_td_init, NULL, &behavior_sm_td_data_##n, &behavior_sm_td_config_##n,    \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_sm_td_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SM_TD_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */