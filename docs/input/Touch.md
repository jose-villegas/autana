# Touch

`input/touch/touch.c` reads the touch controller through `board_touch_open()` and delivers
press and release edges to the frame loop. Both board revisions are handled.
Controller quirks and the interrupt pin are in
[Input-and-Sensors.md](../notes/Input-and-Sensors.md#touch-controller).

## Example

<!-- generated: input-touch-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Touch: count taps, follow the finger. Call from your app's update(dt_ms, input). */
#include "input/input.h"

typedef struct {
    int taps;
    vec2i_t point;
} touch_example_t;

void
touch_example(touch_example_t* s, const input_t* input) {
    if (input->pressed) {
        s->taps++; /* an edge: once per touch, however long it is held */
    }
    if (input->down) {
        s->point = input->point; /* the level: every frame the finger stays down */
    }
}
```
<!-- /generated: input-touch-example -->

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

## Bus wait

The controller shares its I2C bus with the IMU and the PMU, so a read can wait
behind their transactions. A development build logs the reads on the shell's
touch line: count, failures, p95 and worst time, each including that wait.
`autana tune touch.probe 1` reads the controller every poll with no contact,
so an untouched board measures it. An idle controller NACKs these reads, which
are counted as failures; they still wait for the bus first.
