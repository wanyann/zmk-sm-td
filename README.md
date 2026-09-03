# ZMK SM_TD

SM Tap-Dance for ZMK — a ZMK module port of the [QMK SM_TD](https://github.com/stasmarkin/sm_td) library. It makes
Home Row Modifiers (HRMs) and Tap-Dance reliable during fast typing by analyzing key **releases** (not just presses).

This is a clean adaptation of the MIT-licensed QMK `sm_td` state machine to the ZMK behavior / Zephyr event model.

## Overview

SM_TD improves tap vs. hold decisions for `&mt`-style keys by looking at the time between key releases:

- `↓h ↓i ↑h ↑i` (tiny pause) → treat as an overlap: hold/action on `h` + tap `i`
- `↓h ↓i ↑h` (long pause) `↑i` → treat as sequential taps: tap `h` + tap `i`

By default, ZMK's hold-tap resolves on the first *interrupting press*, which misclassifies natural rolls. SM_TD waits
for releases, giving more reliable home-row mods.

## Installation

Add this repository as a module in your `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: <your-username>
      url-base: https://github.com/<your-username>
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: v0.3   # Set to your desired ZMK release
      import: app/west.yml
    - name: zmk-sm-td
      remote: <your-username>
      revision: main
  self:
    path: config
```

## Usage

Include the convenience behavior instances, then use the `SMTD_MT` / `SMTD_LT` macros in your keymap:

```dts
#include <behaviors.dtsi>
#include <dt-bindings/zmk/keys.h>
#include <sm_td.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <
                SMTD_MT(LCTRL, A)    SMTD_MT(LALT, S)
                SMTD_MT(LSHIFT, D)   SMTD_MT(LGUI, F)
            >;
        };

        nav_layer {
            bindings = <
                SMTD_LT(1, SEMICOLON)   ...
            >;
        };
    };
};
```

### Custom behavior definitions

Instead of the provided `&sm_td_mt` / `&sm_td_lt`, you can define your own instances to tune timings per key group:

```dts
/ {
    behaviors {
        home_left: home_row_mod_left {
            compatible = "zmk,behavior-sm-td";
            #binding-cells = <2>;
            bindings = <&mt>, <&kp>;
            tap-term-ms = <200>;
            sequence-term-ms = <100>;
            release-term-ms = <50>;
            release-percent = <30>;
            flavor = "balanced";
        };
    };
};
```

## Behavior properties

| Property             | Type    | Default | Description                                        |
|----------------------|---------|---------|----------------------------------------------------|
| `bindings`           | phandle-array | required | `[hold-behavior, tap-behavior]`          |
| `tap-term-ms`        | int     | 200     | Max tap duration before resolving a hold           |
| `sequence-term-ms`   | int     | 100     | Window for an accumulated sequence of taps         |
| `release-term-ms`    | int     | 50      | Fixed release-decision window                      |
| `release-percent`    | int     | 30      | Dynamic release window percent (0 disables dynamic)|
| `aggregate-taps`     | bool    | false   | Aggregate taps before resolving                    |
| `retro-tap`          | bool    | false   | Emit tap on release when no other key was pressed  |
| `flavor`             | enum    | balanced | `balanced` / `hold-preferred` / `tap-preferred`   |

## How it works

The module registers a listener on ZMK's `position_state_changed` event. Every physical key move is fed into a
per-instance state pool. Each pending key walks the SM_TD state machine (TOUCH → SEQUENCE → HOLD etc.), deciding
between tap/hold based on release timing. When resolved, the corresponding hold/tap binding is invoked through the
standard ZMK behavior pipeline.

## License

MIT — see [LICENSE](LICENSE). The state-machine logic is ported from [sm_td](https://github.com/stasmarkin/sm_td)
(c) Stanislav Markin, MIT.
