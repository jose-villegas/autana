# Touch

`input/touch/touch.c` reads the touch controller through the BSP and delivers
press and release edges to the frame loop. Both board revisions are handled.
Controller quirks and the interrupt pin are in
[Input-and-Sensors.md](../notes/Input-and-Sensors.md#touch-controller).

## Delivery

| Rule | Owner |
|---|---|
| The task polls at `TOUCH_POLL_HZ` and latches press and release edges, so a tap that falls entirely between rendered frames still reaches the next frame | `touch.c` |
| Release needs the quiet interval `TOUCH_RELEASE_QUIET_US`; an INT deassertion is not a release | `input/touch/touch_fsm.h` |
| The UI bridge defers DOWN until microui's hover root is settled; keep this for every control, not only buttons | [firmware/UI.md](../firmware/UI.md#two-things-to-know-before-touching-it) |

## Calibration and targets

`touch.c` holds the panel fit. `input/touch/touch_calib.c` applies its inverse
before consumers receive coordinates. A development build can bypass it with
`autana tune touch.calibrate 0` for comparison.

Calibration corrects systematic distortion, not fingertip scatter. Use large
targets and keep them clear of `DISPLAY_PANEL_CORNER_RADIUS` and
`DISPLAY_PANEL_SAFE_INSET`.

The development touch probe records target and reported coordinates for
fitting `PANEL_FIT` in `touch.c`. Refit from targeted captures rather than
treating one tap's miss as a coefficient error.
