# Shake

![Deliberate shake level from smoothed linear acceleration](../images/input/shake-level.png)

Shake measures the smoothed size of linear acceleration in screen axes.
It reports deliberate shaking only; a gentle shake can overlap walking.
Spinning about the screen normal reads nothing because gravity tracks the rotation.

## Reader

Feed `shake_update()` with `gravity_linear()` in g and elapsed microseconds,
per sample or frame. The one-pole exponential step accepts any dt;
zero dt leaves the state unchanged. `shake_reset()` clears the smoothed size.

`shake_level()` is zero at or below START, rises linearly to one at FULL,
and stays at one above FULL. Direction does not affect the result.

| Constant | Capture constraint |
|---|---|
| `SHAKE_TAU_MS` | Mixed-direction and side-to-side shaking: carries motion between strokes |
| `SHAKE_START_G` | Both protocols: above walking, taps, flips and lifting the board |
| `SHAKE_FULL_G` | Both protocols: hard shaking reaches full level |

The constants in `shake.h` come from guided board captures.

`tilt_shake()` is kept until its remaining callers move to this reader.

## Figure

The figure probes `shake_level()` directly and starts the level axis at zero.
Regenerate it with the host compiler:

```sh
python launcher/tools/input/shake_figures.py --cc cc --out docs/images/input/shake-level.png
```
