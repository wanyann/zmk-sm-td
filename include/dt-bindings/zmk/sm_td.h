/* Copyright 2026 Stanislav Markin (https://github.com/stasmarkin)
 * Copyright 2026 ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Convenience macros for the common SM_TD use-cases. The underlying behavior
 * instances (sm_td_mt / sm_td_lt) are declared in sm_td.dtsi and take the
 * HOLD behavior/param as the first binding parameter and the TAP key as the
 * second. These macros expand to the behavior node name followed by the two
 * parameters, keeping keymap rows terse. Note they do NOT include the leading
 * '&' — reference them in keymaps the same way as other behaviors, e.g.
 * `&SMTD_MT(LEFT_SHIFT, E)`. */

#define SMTD_MT(mod, tap) sm_td_mt mod tap
#define SMTD_LT(layer, tap) sm_td_lt layer tap
#define SMTD_MT_ON_MKEY(mod, tap) sm_td_mt mod tap
#define SMTD_LT_ON_MKEY(layer, tap) sm_td_lt layer tap