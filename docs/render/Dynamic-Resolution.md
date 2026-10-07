# Dynamic resolution

A scene's camera can draw each frame at a render size picked to hold a frame
budget, instead of one fixed scale. The sizes are a ladder of steps, finest
first; the picture is always upscaled back to the panel by `render/upscale.h`,
which takes any ratio in either axis. The code is
`launcher/main/render/resolution/`. It is opt-in, a setting of the render
context rather than of a scene or a camera: an app turns it on with
`render_context_set_dynamic_resolution()` ([Mesh-Rendering.md](Mesh-Rendering.md#the-render-context)),
and with no call every frame draws at the context's fixed scale.

| Policy | Chooses | From | Guards against flapping |
|---|---|---|---|
| Stepped controller | one step at a time, after the cost | the mean of a window of measured frames against an up and a down threshold | a cooldown after each step that doubles when a step reverses the last one; a panic drop for one frame far over budget |
| Predictor | any step, before the cost | a linear model of the frame, priced at every step from the triangles culling kept this frame, corrected by measured frames | going finer needs a margin under the budget |

The cost a policy holds is the part that scales: the draw and the upscale.
The present and whatever an app draws over the scene are not in it. The
predictor culls once with the picture's unfitted lens, before it chooses a
size, and draws from the scratch block's census list with a fitted lens. The
list's offset is independent of render size, so the context reserves only
the finest step's raster block. Fixed and stepped draws share one fitted lens
per instance between culling and drawing.

## Where a frame's time goes at each size

The raster brackets its stages for [frame cost](../tools/Frame-Cost.md):
`r3d.cull`, `r3d.transform`, `r3d.draw` and `r3d.upscale`; through the
predictor the cull is the census, `r3d.census`. The suite behind
these tables draws the test scene's path at every size, both cores, and then
once more on one core with the span rasterizer stopped after each stage.

<!-- generated: dynres-stages sha256=916e390d68345be9ad4664b920175377fa0791b01cb72e256a3411d4bb6a7b20 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 107.1 | 108.3 | 126.8 | 0.6 | 4.3 | 91.7 | 10.4 | 32.9 | 33.5 | 33.5 | 44.8 |
| 294x358 | 1.25 | 64% | 84.2 | 84.6 | 102.3 | 0.6 | 4.4 | 69.4 | 9.7 | 31.3 | 27.3 | 25.6 | 25.7 |
| 276x336 | 1.33 | 56% | 79.5 | 80.0 | 97.4 | 0.6 | 4.4 | 64.7 | 9.8 | 30.9 | 25.7 | 23.6 | 22.5 |
| 245x298 | 1.50 | 44% | 70.9 | 70.8 | 87.2 | 0.6 | 4.4 | 56.8 | 9.0 | 30.1 | 23.0 | 20.4 | 16.2 |
| 210x256 | 1.75 | 33% | 63.7 | 64.7 | 78.1 | 0.6 | 4.4 | 50.0 | 8.6 | 29.1 | 20.0 | 16.8 | 12.3 |
| 184x224 | 2.00 | 25% | 53.4 | 53.9 | 66.1 | 0.6 | 4.4 | 42.4 | 5.9 | 28.1 | 17.7 | 14.2 | 7.3 |
| 147x179 | 2.50 | 16% | 47.5 | 48.0 | 58.9 | 0.7 | 4.4 | 34.6 | 7.8 | 26.5 | 14.3 | 10.6 | 3.2 |
| 122x149 | 3.02 | 11% | 42.4 | 43.0 | 52.6 | 0.7 | 4.4 | 29.8 | 7.6 | 25.2 | 12.1 | 8.3 | 1.2 |
| 368x224 | 1.00 x 2.00 | 50% | 66.4 | 66.4 | 81.8 | 0.6 | 4.4 | 54.0 | 7.3 | 30.7 | 18.9 | 16.6 | 19.0 |
| 184x448 | 2.00 x 1.00 | 50% | 81.8 | 82.6 | 97.0 | 0.6 | 4.4 | 69.5 | 7.3 | 30.8 | 32.0 | 28.8 | 19.6 |
| 245x224 | 1.50 x 2.00 | 33% | 60.5 | 60.6 | 74.8 | 0.6 | 4.4 | 46.7 | 8.7 | 29.1 | 18.2 | 15.2 | 11.3 |
| 184x298 | 2.00 x 1.50 | 33% | 62.7 | 63.4 | 77.0 | 0.6 | 4.4 | 51.3 | 6.4 | 29.1 | 22.5 | 19.0 | 10.7 |

Milliseconds; both cores unless marked one core.
<!-- /generated: dynres-stages -->

The questions the split answers, as differences between two sizes. A size
that keeps the panel's width or halves it upscales whole rows or pixel pairs;
any other width pays the mapped upscale on top, so both ladders keep to those
two widths. Halving the height saves far more than halving the width, since
rows and span setup follow the height, so a ladder cuts the height first. The
floor and the recovery step are named for what they cost, not their scale:
184x179 costs about what isotropic 2.5x (147x179) does, and 184x149 what 3x
(122x149) does, the upscale they save paying for the pixels they keep. The one-core setup stage barely moves with size: it is the
floor no step goes under.

<!-- generated: dynres-findings sha256=799b415266148f4b069ad3b17d0473d5d7b386aede874d2339aa2d73ebda7ae2 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| 1.5x over 2x | 245x298 minus 184x224 | 17.5 | 14.5 | 3.1 | 5.3 | 6.2 | 8.9 |
| 2x over 2.5x | 184x224 minus 147x179 | 5.9 | 7.7 | -1.9 | 3.4 | 3.6 | 4.1 |
| full height over half | 368x448 minus 368x224 | 40.7 | 37.7 | 3.1 | 14.6 | 16.9 | 25.9 |
| full width over half | 368x448 minus 184x448 | 25.3 | 22.3 | 3.1 | 1.5 | 4.7 | 25.2 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew the whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=51f5f9006a93ce6f16cef384f67e8ad56a18b6df360b67670f5c68b53ebfd071 -->
| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|
| 60.0 | fixed | half | 54.0 | 65.7 | 67.8 | 23.7% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 60.0 | stepped | isotropic | 50.9 | 60.8 | 66.1 | 7.9% | 13 | 294x358 1%, 245x298 3%, 210x256 18%, 184x224 28%, 147x179 49% | 9.38 | 0.5506 |
| 60.0 | predicted | isotropic | 54.3 | 59.3 | 63.4 | 2.2% | 14 | 294x358 2%, 245x298 12%, 210x256 18%, 184x224 35%, 147x179 34% | 9.36 | 0.5519 |
| 60.0 | predicted | height | 53.8 | 59.5 | 61.9 | 2.2% | 14 | 368x298 3%, 368x224 16%, 245x224 18%, 184x224 30%, 147x179 33% | 9.34 | 0.5533 |
| 75.0 | fixed | half | 54.0 | 65.7 | 67.8 | 0.0% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 75.0 | stepped | isotropic | 65.0 | 75.0 | 82.1 | 5.0% | 12 | 368x448 2%, 294x358 12%, 245x298 22%, 210x256 38%, 184x224 26% | 9.30 | 0.5565 |
| 75.0 | predicted | isotropic | 65.9 | 72.9 | 77.7 | 1.7% | 10 | 294x358 17%, 245x298 30%, 210x256 35%, 184x224 17% | 9.30 | 0.5568 |
| 75.0 | predicted | height | 65.5 | 74.5 | 80.8 | 3.0% | 14 | 368x448 1%, 368x358 2%, 368x298 24%, 368x224 37%, 245x224 23%, 184x224 11% | 9.26 | 0.5598 |

Frame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM are against the reference at full size, lower dE and higher SSIM being closer.
<!-- /generated: dynres-policies -->

What the table shows:

- **Over budget.** At the tight budget the half scale itself is often over,
  and both policies step down to the floor and stop there: a frame a
  little over budget costs less than a harsh drop in picture. The predictor
  steps on the frame the load arrives, the controller a window later.
- **Switches.** Neither flaps: the cooldown spaces the controller's steps,
  and the finer-step margin keeps the predictor from returning to a step it
  just left.
- **Quality.** The scorer's dE is mostly the bake against the reference, a
  floor every size shares, so the differences are small; the order is what
  counts. Cutting the height first keeps the full width longer and scores
  closer to the reference than cutting the width first at the same budget,
  so the height-first ladder buys more picture for the same milliseconds.
- **Recovery.** The recovery step is reached only past the panic share;
  neither budget here comes near it. Below the floor a step buys little, as
  the setup floor in the stage table shows.

![Frame cost and render size along the path, per policy and budget](../images/render/dynamic-resolution-flight.png)

## Refreshing

The tables and the chart are made by the docs generator,
`launcher/tools/render/render_doc_images.sh`, like every other measured page,
from the board capture and the quality CSV kept beside this page in `data/`.
The capture is the suite's `scale_split`, `scale_spans`, `dynres_step` and
`dynres_frames` lines; the CSV is the test scene's own quality script, which
renders the path at each size on a host and scores it. A new board run
replaces the capture, and the generator does the rest:

```sh
autana suite run_raster_scale_perf_suite --flash
sh launcher/tools/render/render_doc_images.sh
```
