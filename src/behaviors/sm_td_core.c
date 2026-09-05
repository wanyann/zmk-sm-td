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
 *
 * This is a ZMK port of the SM_TD state machine. The decision logic
 * (smtd_apply_event, smtd_apply_stage, smtd_handle_action, smtd_execute_action)
 * is preserved from the original MIT-licensed QMK library, with the QMK
 * timer/record/deferred-exec APIs replaced by their ZMK/Zephyr equivalents.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#define LOG_LEVEL CONFIG_SM_TD_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(sm_td);

#include <zmk/behavior.h>
#include <zmk/keymap.h>

#include <zmk-sm-td/sm_td.h>

/* Out-calls implemented by the behavior driver. */
smtd_resolution smtd_driver_on_action(smtd_runtime *rt, uint32_t position, smtd_action action, uint8_t tap_count);
void smtd_driver_emit_key(smtd_runtime *rt, uint32_t position, bool pressed, int64_t timestamp);
/* Called whenever a resolution pass may have completed; the driver uses it to
 * release captured position events once no instance is undecided anymore. */
void smtd_driver_after_resolve(smtd_runtime *rt);

/* ------------------------------------------------------------------ *
 *                        DEBUG HELPERS                               *
 * ------------------------------------------------------------------ */

static const char *smtd_stage_name(smtd_stage stage) {
    switch (stage) {
    case SMTD_STAGE_NONE: return "NONE";
    case SMTD_STAGE_TOUCH: return "TOUCH";
    case SMTD_STAGE_SEQUENCE: return "SEQ";
    case SMTD_STAGE_HOLD: return "HOLD";
    case SMTD_STAGE_TOUCH_RELEASE: return "T_REL";
    case SMTD_STAGE_HOLD_RELEASE: return "H_REL";
    }
    return "?";
}

static const char *smtd_action_name(smtd_action action) {
    switch (action) {
    case SMTD_ACTION_TOUCH: return "TOUCH";
    case SMTD_ACTION_TAP: return "TAP";
    case SMTD_ACTION_HOLD: return "HOLD";
    case SMTD_ACTION_RELEASE: return "RELEASE";
    }
    return "?";
}

static void smtd_log_state(const struct smtd_runtime *rt, const smtd_state *state, const char *tag) {
    if (state == NULL) {
        LOG_DBG("[%p] %s: <null>", (const void *)rt, tag);
        return;
    }
    LOG_DBG("[%p] %s pos=%u idx=%u/%u stage=%s", (const void *)rt, tag, state->position, state->idx,
            rt->active_size, smtd_stage_name(state->stage));
}

/* ------------------------------------------------------------------ *
 *                          TIMEOUTS                                  *
 * ------------------------------------------------------------------ */

/* The single shared timeout worker. The smtd_timeout struct carries a
 * back-pointer to its owning smtd_state, so we can recover everything we
 * need to resolve the current stage. */
void smtd_timeout_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct smtd_timeout *t = CONTAINER_OF(dwork, struct smtd_timeout, work);
    smtd_runtime *rt = t->runtime;
    smtd_state *state = t->owner;
    if (rt == NULL || state == NULL) {
        return;
    }
    t->active = false;

    smtd_log_state(rt, state, "timeout");

    switch (state->stage) {
    case SMTD_STAGE_TOUCH:
        smtd_apply_stage(rt, state, SMTD_STAGE_HOLD);
        smtd_handle_action(rt, state, SMTD_ACTION_HOLD);
        break;
    case SMTD_STAGE_SEQUENCE:
        if (smtd_feature_enabled_or_default(rt->config, state->position, SMTD_FEATURE_AGGREGATE_TAPS)) {
            smtd_handle_action(rt, state, SMTD_ACTION_TAP);
        }
        smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
        break;
    case SMTD_STAGE_TOUCH_RELEASE:
        smtd_handle_action(rt, state, SMTD_ACTION_TAP);
        smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
        break;
    case SMTD_STAGE_HOLD_RELEASE:
        smtd_handle_action(rt, state, SMTD_ACTION_RELEASE);
        smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
        break;
    default:
        break;
    }

    smtd_driver_after_resolve(rt);
}

static void smtd_schedule_timeout(smtd_runtime *rt, smtd_state *state, uint32_t ms) {
    struct smtd_timeout *t = &state->timeout;
    if (t->active) {
        k_work_cancel_delayable(&t->work);
    }
    t->runtime = rt;
    t->owner = state;
    t->active = true;
    k_work_schedule(&t->work, K_MSEC(ms));
}

static void smtd_cancel_timeout(struct smtd_timeout *t) {
    if (t->active) {
        k_work_cancel_delayable(&t->work);
        t->active = false;
    }
}

/* ------------------------------------------------------------------ *
 *                     STATE ALLOCATION                               *
 * ------------------------------------------------------------------ */

static bool smtd_is_state_key(smtd_runtime *rt, uint32_t position, smtd_state *state) {
    (void)rt;
    return state->position == position;
}

static void smtd_reset_state(smtd_state *state) {
    state->position = 0;
    state->pressed_time = 0;
    state->released_time = 0;
    state->release_term = 0;
    state->timeout.active = false;
    state->timeout.runtime = NULL;
    state->timeout.owner = NULL;
    state->stage = SMTD_STAGE_NONE;
    state->resolution = SMTD_RESOLUTION_UNCERTAIN;
    state->action_performed = -1;
    state->action_required = -1;
    state->idx = 0;
    state->tap_count = 0;
}

void smtd_reset_runtime(smtd_runtime *runtime) {
    for (uint8_t i = 0; i < SMTD_POOL_SIZE; i++) {
        if (runtime->pool[i].timeout.active) {
            k_work_cancel_delayable(&runtime->pool[i].timeout.work);
        }
        k_work_init_delayable(&runtime->pool[i].timeout.work, smtd_timeout_cb);
        runtime->pool[i].timeout.active = false;
        runtime->pool[i].timeout.runtime = NULL;
        runtime->pool[i].timeout.owner = NULL;
        smtd_reset_state(&runtime->pool[i]);
        runtime->active[i] = NULL;
    }
    runtime->active_size = 0;
    runtime->bypass = false;
    runtime->emitting = false;
    runtime->sync_emit = false;
    runtime->releasing_captured = false;
    runtime->captured_size = 0;
    for (uint8_t i = 0; i < SMTD_CAPTURED_EVENTS_SIZE; i++) {
        runtime->captured[i].used = false;
    }
}

/* ------------------------------------------------------------------ *
 *                      STATE PROCESSING                              *
 * ------------------------------------------------------------------ */

bool smtd_process_event(smtd_runtime *rt, uint32_t position, int64_t pressed_time, int64_t released_time,
                        bool pressed) {
    if (rt->bypass) {
        return true;
    }
    smtd_apply_to_stack(rt, 0, position, pressed_time, pressed, released_time, 0);
    return false;
}

void smtd_apply_to_stack(smtd_runtime *rt, uint8_t starting_idx, uint32_t position, int64_t pressed_time,
                         const bool pressed, int64_t released_time, uint8_t tap_count) {
    bool processed_state = false;

    for (uint8_t i = starting_idx; i < rt->active_size; i++) {
        smtd_state *state = rt->active[i];

        bool is_state_key = smtd_is_state_key(rt, position, state);
        processed_state = processed_state | is_state_key;

        smtd_apply_event(rt, is_state_key, state, position, pressed_time, released_time, pressed, tap_count);

        if (state->stage == SMTD_STAGE_NONE) {
            LOG_DBG("[%p] apply_to_stack: pos=%u resolved, reprocess @i=%u (active_size=%u)", (const void *)rt,
                    position, i, rt->active_size);
            if (i > 0) {
                i--;
            }
        }
    }

    uint8_t idx = rt->active_size;
    uint8_t guard_iterations = 0;
    while (idx > 0) {
        /* Defensive: if a stage->NONE removal ever fails to shrink
         * active_size (e.g. a MISMATCH under a combo re-raise), this loop
         * could spin forever. Cap the number of passes and bail out. */
        if (++guard_iterations > (SMTD_POOL_SIZE * 4)) {
            LOG_ERR("[%p] apply_to_stack: iteration guard tripped, force resolving active stack "
                    "(active_size=%u)",
                    (const void *)rt, rt->active_size);
            for (uint8_t k = 0; k < rt->active_size; k++) {
                smtd_state *s = rt->active[k];
                smtd_handle_action(rt, s, SMTD_ACTION_TAP);
                smtd_apply_stage(rt, s, SMTD_STAGE_NONE);
            }
            break;
        }
        smtd_state *state = rt->active[idx - 1];
        if (state->stage == SMTD_STAGE_TOUCH_RELEASE) {
            smtd_handle_action(rt, state, SMTD_ACTION_TAP);
            smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
            idx = rt->active_size;
            continue;
        }
        if (state->stage == SMTD_STAGE_HOLD_RELEASE) {
            smtd_handle_action(rt, state, SMTD_ACTION_RELEASE);
            smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
            idx = rt->active_size;
            continue;
        }
        if (state->stage == SMTD_STAGE_SEQUENCE) {
            idx--;
            continue;
        }
        break;
    }

    if (processed_state) {
        smtd_driver_after_resolve(rt);
        return;
    }
    if (!pressed) {
        smtd_driver_after_resolve(rt);
        return;
    }
    smtd_create_state(rt, position, pressed_time, released_time, pressed, tap_count);
    smtd_driver_after_resolve(rt);
}

void smtd_create_state(smtd_runtime *rt, uint32_t position, int64_t pressed_time, int64_t released_time,
                       bool pressed, uint8_t tap_count) {
    smtd_state *state = NULL;
    for (uint8_t i = 0; i < SMTD_POOL_SIZE; i++) {
        if (rt->pool[i].stage == SMTD_STAGE_NONE) {
            state = &rt->pool[i];
            break;
        }
    }
    if (state == NULL) {
        return;
    }

    rt->active[rt->active_size] = state;
    state->idx = rt->active_size;
    state->position = position;
    state->pressed_time = pressed_time;
    rt->active_size++;

    smtd_apply_event(rt, true, state, position, pressed_time, released_time, pressed, tap_count);
}

bool is_following_key(smtd_runtime *rt, smtd_state *state, uint32_t position) {
    for (uint8_t i = state->idx + 1; i < rt->active_size; i++) {
        if (smtd_is_state_key(rt, position, rt->active[i])) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ *
 *                    EVENT APPLICATION                               *
 * ------------------------------------------------------------------ */

void smtd_apply_event(smtd_runtime *rt, bool is_state_key, smtd_state *state, uint32_t position,
                      int64_t pressed_time, int64_t released_time, bool event_pressed, uint8_t tap_count) {
    (void)pressed_time;
    (void)released_time;
    (void)tap_count;

    switch (state->stage) {
    case SMTD_STAGE_NONE: {
        if (is_state_key && event_pressed) {
            smtd_apply_stage(rt, state, SMTD_STAGE_TOUCH);
            smtd_handle_action(rt, state, SMTD_ACTION_TOUCH);
        }
        break;
    }

    case SMTD_STAGE_TOUCH: {
        if (state->idx + 1 == rt->active_size) {
            if (is_state_key && !event_pressed) {
                if (!smtd_feature_enabled_or_default(rt->config, state->position, SMTD_FEATURE_AGGREGATE_TAPS)) {
                    smtd_handle_action(rt, state, SMTD_ACTION_TAP);
                }
                smtd_apply_stage(rt, state, SMTD_STAGE_SEQUENCE);
                break;
            }
            break;
        }

        if (is_state_key && !event_pressed) {
            smtd_apply_stage(rt, state, SMTD_STAGE_TOUCH_RELEASE);
            break;
        }

        if (!is_following_key(rt, state, position)) {
            break;
        }

        if (!is_state_key && !event_pressed) {
            smtd_apply_stage(rt, state, SMTD_STAGE_HOLD);
            smtd_handle_action(rt, state, SMTD_ACTION_HOLD);
            break;
        }
        break;
    }

    case SMTD_STAGE_SEQUENCE: {
        if (is_state_key && event_pressed) {
            state->tap_count++;
            state->action_performed = -1;
            state->action_required = -1;
            smtd_handle_action(rt, state, SMTD_ACTION_TOUCH);
            smtd_apply_stage(rt, state, SMTD_STAGE_TOUCH);
            break;
        }

        if (!is_state_key && event_pressed) {
            state->resolution = SMTD_RESOLUTION_DETERMINED;
            if (smtd_feature_enabled_or_default(rt->config, state->position, SMTD_FEATURE_AGGREGATE_TAPS)) {
                smtd_handle_action(rt, state, SMTD_ACTION_TAP);
            }
            smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
            break;
        }
        break;
    }

    case SMTD_STAGE_HOLD: {
        if (is_state_key && !event_pressed) {
            if (state->idx == rt->active_size - 1) {
                smtd_handle_action(rt, state, SMTD_ACTION_RELEASE);
                smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
                break;
            }
            smtd_apply_stage(rt, state, SMTD_STAGE_HOLD_RELEASE);
            break;
        }
        break;
    }

    case SMTD_STAGE_TOUCH_RELEASE: {
        if (event_pressed) {
            smtd_handle_action(rt, state, SMTD_ACTION_TAP);
            smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
            break;
        }

        if ((k_uptime_get() - state->released_time) >= state->release_term) {
            smtd_handle_action(rt, state, SMTD_ACTION_TAP);
            smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
            break;
        }

        if (!is_following_key(rt, state, position)) {
            break;
        }

        smtd_apply_stage(rt, state, SMTD_STAGE_HOLD_RELEASE);
        smtd_handle_action(rt, state, SMTD_ACTION_HOLD);
        break;
    }

    case SMTD_STAGE_HOLD_RELEASE: {
        if (!event_pressed && state->idx != rt->active_size - 1) {
            break;
        }
        smtd_handle_action(rt, state, SMTD_ACTION_RELEASE);
        smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
        break;
    }
    }
}

/* ------------------------------------------------------------------ *
 *                     STAGE TRANSITIONS                              *
 * ------------------------------------------------------------------ */

void smtd_apply_stage(smtd_runtime *rt, smtd_state *state, smtd_stage next_stage) {
    smtd_cancel_timeout(&state->timeout);
    state->stage = next_stage;

    switch (state->stage) {
    case SMTD_STAGE_NONE:
        if (state->idx < rt->active_size && rt->active[state->idx] == state) {
            for (uint8_t j = state->idx; j + 1 < rt->active_size; j++) {
                rt->active[j] = rt->active[j + 1];
                rt->active[j]->idx--;
            }
            rt->active_size--;
            rt->active[rt->active_size] = NULL;
            LOG_DBG("[%p] stage->NONE removed pos=%u idx=%u, active_size=%u", (const void *)rt,
                    state->position, state->idx, rt->active_size);
        } else {
            LOG_ERR("[%p] stage->NONE pos=%u idx=%u MISMATCH vs active_size=%u (rt->active[%u]=%p state=%p)",
                    (const void *)rt, state->position, state->idx, rt->active_size, state->idx,
                    (state->idx < rt->active_size) ? (void *)rt->active[state->idx] : (void *)NULL,
                    (const void *)state);
        }
        smtd_reset_state(state);
        break;

    case SMTD_STAGE_TOUCH:
        state->pressed_time = k_uptime_get();
        smtd_schedule_timeout(rt, state,
                              get_smtd_timeout_or_default(rt->config, state->position, SMTD_TIMEOUT_TAP));
        break;

    case SMTD_STAGE_SEQUENCE:
        state->released_time = k_uptime_get();
        state->resolution = SMTD_RESOLUTION_UNCERTAIN;
        smtd_schedule_timeout(rt, state,
                              get_smtd_timeout_or_default(rt->config, state->position, SMTD_TIMEOUT_SEQUENCE));
        break;

    case SMTD_STAGE_HOLD:
        break;

    case SMTD_STAGE_TOUCH_RELEASE:
        state->released_time = k_uptime_get();
        state->release_term = smtd_compute_release_term(rt, state);
        smtd_schedule_timeout(rt, state, state->release_term);
        break;

    case SMTD_STAGE_HOLD_RELEASE:
        state->released_time = k_uptime_get();
        state->release_term = smtd_compute_release_term(rt, state);
        smtd_schedule_timeout(rt, state, state->release_term);
        break;
    }
}

/* ------------------------------------------------------------------ *
 *                      ACTION RESOLUTION                             *
 * ------------------------------------------------------------------ */

void smtd_handle_action(smtd_runtime *rt, smtd_state *state, smtd_action action) {
    if (state->action_required == -1 || action > state->action_required) {
        state->action_required = action;
    }

    if (state->action_performed > 0 && state->action_performed >= action) {
        return;
    }

    if (smtd_worst_resolution_before(rt, state) < SMTD_RESOLUTION_DETERMINED) {
        return;
    }

    LOG_DBG("[%p] action: pos=%u action=%s idx=%u/%u req=%d done=%d", (const void *)rt, state->position,
            smtd_action_name(action), state->idx, rt->active_size, state->action_required,
            state->action_performed);
    smtd_resolution resolution_before = state->resolution;
    smtd_execute_action(rt, state, action);
    state->action_performed = action;

    if (resolution_before == SMTD_RESOLUTION_DETERMINED) {
        return;
    }
    if (state->resolution != SMTD_RESOLUTION_DETERMINED) {
        return;
    }

    for (int i = state->idx + 1; i < rt->active_size; i++) {
        smtd_state *next_state = rt->active[i];
        if (next_state->action_required == -1) {
            break;
        }
        if (next_state->action_performed != -1 && next_state->action_performed >= next_state->action_required) {
            break;
        }
        switch (next_state->action_required) {
        case SMTD_ACTION_TOUCH:
            smtd_handle_action(rt, next_state, SMTD_ACTION_TOUCH);
            break;
        case SMTD_ACTION_TAP:
            smtd_handle_action(rt, next_state, SMTD_ACTION_TOUCH);
            smtd_handle_action(rt, next_state, SMTD_ACTION_TAP);
            break;
        case SMTD_ACTION_HOLD:
            smtd_handle_action(rt, next_state, SMTD_ACTION_TOUCH);
            smtd_handle_action(rt, next_state, SMTD_ACTION_HOLD);
            break;
        case SMTD_ACTION_RELEASE:
            smtd_handle_action(rt, next_state, SMTD_ACTION_TOUCH);
            smtd_handle_action(rt, next_state, SMTD_ACTION_HOLD);
            smtd_handle_action(rt, next_state, SMTD_ACTION_RELEASE);
            break;
        }
    }
}

void smtd_execute_action(smtd_runtime *rt, smtd_state *state, smtd_action action) {
    smtd_resolution resolution = smtd_driver_on_action(rt, state->position, action, state->tap_count);

    if (resolution > state->resolution) {
        state->resolution = resolution;
    }

    if (resolution == SMTD_RESOLUTION_UNHANDLED) {
        switch (action) {
        case SMTD_ACTION_TOUCH:
            smtd_driver_emit_key(rt, state->position, true, k_uptime_get());
            state->resolution = SMTD_RESOLUTION_DETERMINED;
            break;
        case SMTD_ACTION_TAP:
            smtd_driver_emit_key(rt, state->position, false, k_uptime_get());
            break;
        case SMTD_ACTION_HOLD:
            break;
        case SMTD_ACTION_RELEASE:
            smtd_driver_emit_key(rt, state->position, false, k_uptime_get());
            break;
        }
    }
}

/* ------------------------------------------------------------------ *
 *                          UTILITIES                                 *
 * ------------------------------------------------------------------ */

smtd_resolution smtd_worst_resolution_before(smtd_runtime *rt, smtd_state *state) {
    smtd_resolution result = SMTD_RESOLUTION_DETERMINED;
    for (uint8_t i = 0; i < state->idx; i++) {
        if (rt->active[i]->stage == SMTD_STAGE_SEQUENCE) {
            continue;
        }
        if (rt->active[i]->resolution < result) {
            result = rt->active[i]->resolution;
        }
    }
    return result;
}

uint32_t get_smtd_timeout_default(const struct smtd_config *config, smtd_timeout timeout) {
    switch (timeout) {
    case SMTD_TIMEOUT_TAP:
        return config->tap_term_ms;
    case SMTD_TIMEOUT_SEQUENCE:
        return config->sequence_term_ms;
    case SMTD_TIMEOUT_RELEASE:
        return config->release_term_ms;
    }
    return 0;
}

uint32_t get_smtd_timeout_or_default(const struct smtd_config *config, uint32_t position, smtd_timeout timeout) {
    (void)position;
    return get_smtd_timeout_default(config, timeout);
}

uint32_t smtd_compute_release_term(smtd_runtime *rt, smtd_state *state) {
    uint32_t fixed_term = get_smtd_timeout_or_default(rt->config, state->position, SMTD_TIMEOUT_RELEASE);

    if (rt->config->release_percent > 0) {
        if (state->idx + 1 >= rt->active_size) {
            return fixed_term;
        }
        smtd_state *next = rt->active[state->idx + 1];
        int64_t p1 = next->pressed_time - state->pressed_time;
        int64_t p2 = state->released_time - next->pressed_time;
        int64_t min_pause = (p1 < p2 ? p1 : p2);
        if (min_pause < 1) {
            min_pause = 1;
        }
        uint32_t term = (uint32_t)(min_pause * rt->config->release_percent / 100);
        if (term < 1) {
            term = 1;
        }
        if (term > fixed_term) {
            term = fixed_term;
        }
        return term;
    }
    return fixed_term;
}

bool smtd_feature_enabled_default(const struct smtd_config *config, smtd_feature feature) {
    switch (feature) {
    case SMTD_FEATURE_AGGREGATE_TAPS:
        return config->aggregate_taps;
    }
    return false;
}

bool smtd_feature_enabled_or_default(const struct smtd_config *config, uint32_t position, smtd_feature feature) {
    (void)position;
    return smtd_feature_enabled_default(config, feature);
}

/* ------------------------------------------------------------------ *
 *                      ZMK CAPTURE SUPPORT                            *
 * ------------------------------------------------------------------ */

void smtd_other_key_down(smtd_runtime *rt, uint32_t position) {
    (void)position;
    for (uint8_t i = 0; i < rt->active_size; i++) {
        smtd_state *state = rt->active[i];
        if (state->resolution != SMTD_RESOLUTION_UNCERTAIN) {
            continue;
        }
        if (state->stage == SMTD_STAGE_TOUCH) {
            /* An external keypress while this key is still undecided means the
             * user is holding it to use a modifier: commit to hold immediately
             * so the external key can flow straight through to the keymap.
             * Emit synchronously: the modifier must be registered before the
             * external key's own keymap handling in the same event dispatch,
             * otherwise the letter is reported without it. */
            LOG_DBG("[%p] external press resolves pos=%u to HOLD", (const void *)rt, state->position);
            rt->sync_emit = true;
            smtd_apply_stage(rt, state, SMTD_STAGE_HOLD);
            smtd_handle_action(rt, state, SMTD_ACTION_HOLD);
            rt->sync_emit = false;
        } else if (state->stage == SMTD_STAGE_SEQUENCE) {
            /* A later keypress confirms the tap; drop the pending state. */
            smtd_apply_stage(rt, state, SMTD_STAGE_NONE);
        }
    }
}

bool smtd_has_undecided(smtd_runtime *rt) {
    if (rt->bypass || rt->emitting) {
        return false;
    }
    for (uint8_t i = 0; i < rt->active_size; i++) {
        if (rt->active[i]->resolution == SMTD_RESOLUTION_UNCERTAIN) {
            return true;
        }
    }
    return false;
}

bool smtd_owns_position(smtd_runtime *rt, uint32_t position) {
    for (uint8_t i = 0; i < rt->active_size; i++) {
        if (rt->active[i]->position == position) {
            return true;
        }
    }
    return false;
}

int smtd_capture_event(smtd_runtime *rt, uint32_t position, bool pressed) {
    if (rt->captured_size >= SMTD_CAPTURED_EVENTS_SIZE) {
        return -ENOMEM;
    }
    struct smtd_captured_event *ev = &rt->captured[rt->captured_size++];
    ev->used = true;
    ev->position = position;
    ev->pressed = pressed;
    return 0;
}

void smtd_capture_clear(smtd_runtime *rt) {
    for (uint8_t i = 0; i < rt->captured_size; i++) {
        rt->captured[i].used = false;
    }
    rt->captured_size = 0;
}