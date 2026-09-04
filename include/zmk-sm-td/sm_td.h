/* Copyright 2025 Stanislav Markin (https://github.com/stasmarkin)
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
 * Version: 0.1.0 (ZMK port)
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>

/* ************************************* *
 *         BASE DEFINITIONS              *
 * ************************************* */

typedef enum {
    SMTD_STAGE_NONE,
    SMTD_STAGE_TOUCH,
    SMTD_STAGE_SEQUENCE,
    SMTD_STAGE_HOLD,
    SMTD_STAGE_TOUCH_RELEASE,
    SMTD_STAGE_HOLD_RELEASE,
} smtd_stage;

typedef enum {
    SMTD_ACTION_TOUCH,
    SMTD_ACTION_TAP,
    SMTD_ACTION_HOLD,
    SMTD_ACTION_RELEASE,
} smtd_action;

typedef enum {
    SMTD_RESOLUTION_UNCERTAIN,
    SMTD_RESOLUTION_UNHANDLED,
    SMTD_RESOLUTION_DETERMINED,
} smtd_resolution;

typedef enum {
    SMTD_TIMEOUT_TAP,
    SMTD_TIMEOUT_SEQUENCE,
    SMTD_TIMEOUT_RELEASE,
} smtd_timeout;

typedef enum {
    SMTD_FEATURE_AGGREGATE_TAPS,
} smtd_feature;

/* Forward declaration (struct smtd_timeout holds a pointer to it). */
typedef struct smtd_runtime smtd_runtime;

/* Holds the deferred work item used as the state's timer. The back-pointer to
 * the owning runtime lets the shared timeout handler recover both the runtime
 * and the stage to resolve, without any global registry. */
struct smtd_timeout {
    struct k_work_delayable work;
    smtd_runtime *runtime;
    void *owner;
    bool active;
};

typedef struct {
    /** The position of a key that ZMK thinks was pressed */
    uint32_t position;

    /** The timestamp when the key was pressed (ms) */
    int64_t pressed_time;

    /** The timestamp when the key was released (ms) */
    int64_t released_time;

    /** The decision window for the touch-release stage, computed on entering it */
    uint32_t release_term;

    /** The timeout of current stage */
    struct smtd_timeout timeout;

    /** The current stage of the state */
    smtd_stage stage;

    /** The level of certainty of the state */
    smtd_resolution resolution;

    /** The action that already performed */
    int8_t action_performed;

    /** The action that can be performed */
    int8_t action_required;

    /** The index of the state in the active states array */
    uint8_t idx;

    /** The length of the sequence of same key taps */
    uint8_t tap_count;
} smtd_state;

#ifndef SMTD_POOL_SIZE
#define SMTD_POOL_SIZE 10
#endif

/* Per-instance config. Filled by the behavior driver from devicetree. */
struct smtd_config {
    uint32_t tap_term_ms;
    uint32_t sequence_term_ms;
    uint32_t release_term_ms;
    uint32_t release_percent;
    bool aggregate_taps;
};

/* A position_state_changed event held back while an sm_td key is undecided,
 * re-raised once the decision is made. Mirrors the ZMK hold-tap capture. */
struct smtd_captured_event {
    bool used;
    uint32_t position;
    bool pressed;
};

#ifndef SMTD_CAPTURED_EVENTS_SIZE
#define SMTD_CAPTURED_EVENTS_SIZE 32
#endif

struct smtd_runtime {
    smtd_state pool[SMTD_POOL_SIZE];
    smtd_state *active[SMTD_POOL_SIZE];
    uint8_t active_size;
    bool bypass;
    /* Set while the driver is emitting a resolved hold/tap binding, so the
     * listener ignores the re-entrant position events (mirrors QMK bypass). */
    bool emitting;
    const struct smtd_config *config;
    /* Owning behavior device, so the driver can reach its config/API. */
    const struct device *device;
    /* Events captured while this instance had undecided states. */
    struct smtd_captured_event captured[SMTD_CAPTURED_EVENTS_SIZE];
    uint8_t captured_size;
    /* Set while captured events are being re-raised, to avoid recursion. */
    bool releasing_captured;
};

/* ************************************* *
 *           PUBLIC FUNCTIONS            *
 * ************************************* */

/**
 * Process a key press/release event for a position.
 * Called by the behavior driver and the event listener.
 * Returns true when the key was NOT handled by sm_td (should be passed through).
 */
bool smtd_process_event(smtd_runtime *runtime, uint32_t position, int64_t pressed_time, int64_t released_time,
                        bool pressed);

/* Clears all sm_td runtime state for a given instance. */
void smtd_reset_runtime(smtd_runtime *runtime);

/**
 * Resolve the action for a given key/action. Implemented by the user's keymap
 * behavior driver. Overridable.
 */
smtd_resolution smtd_on_action(smtd_runtime *runtime, uint32_t position, smtd_action action, uint8_t tap_count);

/**
 * Get per-key timeout override. Returns 0 to use defaults.
 */
uint32_t smtd_get_timeout(smtd_runtime *runtime, uint32_t position, smtd_timeout timeout);

/**
 * Feature enable override per position.
 */
bool smtd_feature_enabled(smtd_runtime *runtime, uint32_t position, smtd_feature feature);

/* ************************************* *
 *           INTERNAL FUNCTIONS          *
 * ************************************* */

void smtd_apply_to_stack(smtd_runtime *rt, uint8_t starting_idx, uint32_t position, int64_t pressed_time,
                         const bool pressed, int64_t released_time, uint8_t tap_count);

void smtd_create_state(smtd_runtime *rt, uint32_t position, int64_t pressed_time, int64_t released_time,
                       bool pressed, uint8_t tap_count);

bool is_following_key(smtd_runtime *rt, smtd_state *state, uint32_t position);

void smtd_apply_event(smtd_runtime *rt, bool is_state_key, smtd_state *state, uint32_t position,
                      int64_t pressed_time, int64_t released_time, bool event_pressed, uint8_t tap_count);

void smtd_apply_stage(smtd_runtime *rt, smtd_state *state, smtd_stage next_stage);

void smtd_handle_action(smtd_runtime *rt, smtd_state *state, smtd_action action);

void smtd_execute_action(smtd_runtime *rt, smtd_state *state, smtd_action action);

smtd_resolution smtd_worst_resolution_before(smtd_runtime *rt, smtd_state *state);

uint32_t get_smtd_timeout_or_default(const struct smtd_config *config, uint32_t position, smtd_timeout timeout);

uint32_t get_smtd_timeout_default(const struct smtd_config *config, smtd_timeout timeout);

uint32_t smtd_compute_release_term(smtd_runtime *rt, smtd_state *state);

bool smtd_feature_enabled_or_default(const struct smtd_config *config, uint32_t position, smtd_feature feature);

bool smtd_feature_enabled_default(const struct smtd_config *config, smtd_feature feature);

/* Default timeout callbacks (implemented in sm_td_core.c). */
void smtd_timeout_cb(struct k_work *work);

/* ------------------------------------------------------------------ *
 *              DRIVER OUT-CALLS (implemented in behavior_sm_td.c)      *
 * ------------------------------------------------------------------ */

/* Called by core on each action (touch, tap, hold, release). Returns
 * UNHANDLED to let the core invoke emit_key instead. */
smtd_resolution smtd_driver_on_action(smtd_runtime *rt, uint32_t position,
                                      smtd_action action, uint8_t tap_count);

/* Emit a tap press or release via the tap sub-behavior. */
void smtd_driver_emit_key(smtd_runtime *rt, uint32_t position, bool pressed,
                          int64_t timestamp);

/* Called by core after resolving hold/tap. Checks if any instance still has
 * undecided states; if not, re-raises all captured events. */
void smtd_driver_after_resolve(smtd_runtime *rt);

/* ------------------------------------------------------------------ *
 *                 ZMK CAPTURE SUPPORT (implemented in core)           *
 * ------------------------------------------------------------------ */

/* True when the instance has any active, not-yet-fully-resolved state. */
bool smtd_has_undecided(smtd_runtime *rt);

/* True when `position` is currently tracked as one of this instance's own
 * sm_td keys (so the listener must let it bubble to the keymap). */
bool smtd_owns_position(smtd_runtime *rt, uint32_t position);

/* Store a captured position event for later re-raising. Returns -ENOMEM when
 * the capture buffer is full. */
int smtd_capture_event(smtd_runtime *rt, uint32_t position, bool pressed);

/* Re-raise all captured events. `release_fn` is invoked by the core whenever a
 * captured event is ready to be re-raised (keeps the driver decoupled from
 * raising when it wants). Returns the number of events released. */
void smtd_capture_clear(smtd_runtime *rt);