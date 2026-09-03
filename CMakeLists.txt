# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_include_directories(app PRIVATE include)

if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SM_TD
    app PRIVATE src/behaviors/behavior_sm_td.c src/behaviors/sm_td_core.c)
endif()
