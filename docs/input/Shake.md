# Shake

![Deliberate shake level from smoothed linear acceleration](../images/input/shake-level.png)

Shake measures the smoothed size of linear acceleration in screen axes.
It reports deliberate shaking only; a gentle shake can overlap walking.
Spinning about the screen normal reads nothing because gravity tracks the rotation.

## Reader

Feed `shake_update()` with `gravity_linear()` in g and elapsed microseconds,
per sample or per frame. A one-pole low-pass uses the exponential step for dt;
intervals are capped at `GRAVITY_MAX_DT_US`. Zero dt leaves the state unchanged.
`shake_reset()` clears the smoothed size.

`shake_level()` is zero at or below START, rises linearly to one at FULL,
and stays at one above FULL. Direction does not affect the result.

| Constant | Value | Capture constraint |
|---|---|---|
| `SHAKE_TAU_MS` | 120 ms | Mixed-direction and side-to-side shaking: carries motion between strokes |
| `SHAKE_START_MG` | 500 mg | Both protocols: above walking, taps, flips and lifting the board |
| `SHAKE_FULL_MG` | 1500 mg | Both protocols: hard shaking reaches full level |

The development guided shake protocol retains mixed-direction and side-to-side
transcripts, with per-step median, p95, p99 and maximum results beside its tool.
The smoothed size separates deliberate shaking; gentle shaking overlaps walking.

`tilt_shake()` remains until gravity's M4 removes it.

## Figure

The figure probes `shake_level()` directly and starts the level axis at zero.
Regenerate it with the host compiler:

```sh
python launcher/tools/input/shake_figures.py --cc cc --out docs/images/input/shake-level.png
```
