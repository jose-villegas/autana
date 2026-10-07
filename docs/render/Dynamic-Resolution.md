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

<!-- generated: dynres-stages sha256=626f185dacdc38357ec4f73079671705e20598f87e5207ef003755c31d036c72 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 115.2 | 114.9 | 135.1 | 0.7 | 4.3 | 99.8 | 10.4 | 33.6 | 34.5 | 35.0 | 54.3 |
| 184x448 | 2.00 x 1.00 | 50% | 87.2 | 87.6 | 103.1 | 0.7 | 4.3 | 74.7 | 7.5 | 31.5 | 33.1 | 30.4 | 23.5 |
| 184x358 | 2.00 x 1.25 | 40% | 74.1 | 74.6 | 88.1 | 0.7 | 4.3 | 62.3 | 6.7 | 30.6 | 27.2 | 24.2 | 16.7 |
| 184x298 | 2.00 x 1.50 | 33% | 65.9 | 66.5 | 79.2 | 0.7 | 4.3 | 54.5 | 6.3 | 29.9 | 23.2 | 20.1 | 13.0 |
| 184x224 | 2.00 | 25% | 56.0 | 56.6 | 68.4 | 0.7 | 4.3 | 45.0 | 5.9 | 28.8 | 18.2 | 15.0 | 8.9 |
| 184x179 | 2.00 x 2.50 | 20% | 50.0 | 50.5 | 61.3 | 0.7 | 4.4 | 39.4 | 5.6 | 28.0 | 15.2 | 11.9 | 6.1 |
| 184x149 | 2.00 x 3.01 | 17% | 45.8 | 46.6 | 56.2 | 0.7 | 4.3 | 35.3 | 5.4 | 27.4 | 13.1 | 9.9 | 4.5 |
| 368x358 | 1.00 x 1.25 | 80% | 97.0 | 96.1 | 115.5 | 0.7 | 4.3 | 82.8 | 9.1 | 32.8 | 28.5 | 27.9 | 41.4 |
| 368x298 | 1.00 x 1.50 | 67% | 84.9 | 84.6 | 101.6 | 0.7 | 4.3 | 71.5 | 8.3 | 32.3 | 24.5 | 23.1 | 33.2 |
| 368x224 | 1.00 x 2.00 | 50% | 70.4 | 70.2 | 84.3 | 0.7 | 4.3 | 58.0 | 7.3 | 31.4 | 19.4 | 17.4 | 23.3 |
| 294x358 | 1.25 | 64% | 89.5 | 89.4 | 107.0 | 0.7 | 4.3 | 74.8 | 9.7 | 32.1 | 28.1 | 26.8 | 31.0 |
| 276x336 | 1.33 | 56% | 84.3 | 84.4 | 100.9 | 0.7 | 4.3 | 69.5 | 9.7 | 31.7 | 26.5 | 24.8 | 27.2 |
| 245x298 | 1.50 | 44% | 74.6 | 74.6 | 90.0 | 0.7 | 4.3 | 60.6 | 9.0 | 30.8 | 23.8 | 21.5 | 19.4 |
| 210x256 | 1.75 | 33% | 66.6 | 67.1 | 80.1 | 0.7 | 4.3 | 52.9 | 8.6 | 29.8 | 20.7 | 17.8 | 14.6 |
| 245x224 | 1.50 x 2.00 | 33% | 63.5 | 63.6 | 77.3 | 0.7 | 4.3 | 49.7 | 8.7 | 29.7 | 18.8 | 16.0 | 13.7 |
| 147x179 | 2.50 | 16% | 49.5 | 49.4 | 59.9 | 0.7 | 4.3 | 36.6 | 7.7 | 27.2 | 14.8 | 11.3 | 3.9 |
| 122x149 | 3.02 | 11% | 44.0 | 44.4 | 53.2 | 0.7 | 4.4 | 31.4 | 7.5 | 25.9 | 12.5 | 8.8 | 1.5 |

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

<!-- generated: dynres-findings sha256=ba33df7a4543d9136a938f8b8c6ecbdebeeebbd449b2f1393c16fb12d51d0033 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| half width, more height over half | 184x298 minus 184x224 | 9.9 | 9.5 | 0.5 | 5.0 | 5.1 | 4.0 |
| half over floor | 184x224 minus 184x179 | 6.0 | 5.7 | 0.3 | 3.0 | 3.1 | 2.8 |
| floor over 2.5x cost reference | 184x179 minus 147x179 | 0.6 | 2.7 | -2.1 | 0.4 | 0.7 | 2.2 |
| recovery over 3x cost reference | 184x149 minus 122x149 | 1.8 | 3.9 | -2.1 | 0.6 | 1.1 | 2.9 |
| full height over half | 368x448 minus 368x224 | 44.8 | 41.7 | 3.1 | 15.1 | 17.6 | 31.0 |
| full width over half | 368x448 minus 184x448 | 28.0 | 25.1 | 2.9 | 1.5 | 4.6 | 30.8 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew the whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=90fe1b45e78b21a8cc033727a666c75c0c737ed3a690c88a4f4b7dde399d386a -->
| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|
| 60.0 | fixed | half | 56.0 | 68.1 | 70.2 | 36.6% | 0 | 184x224 100% | 9.59 | 0.5478 |
| 60.0 | stepped | width | 53.7 | 61.7 | 69.2 | 14.6% | 11 | 184x358 2%, 184x298 15%, 184x224 23%, 184x179 60% | 9.61 | 0.5465 |
| 60.0 | predicted | width | 54.6 | 61.4 | 62.6 | 13.5% | 12 | 184x358 10%, 184x298 15%, 184x224 29%, 184x179 47% | 9.61 | 0.5468 |
| 60.0 | predicted | height | 54.2 | 61.5 | 62.6 | 12.9% | 8 | 368x224 13%, 184x224 40%, 184x179 47% | 9.60 | 0.5475 |
| 75.0 | fixed | half | 56.0 | 68.1 | 70.2 | 0.0% | 0 | 184x224 100% | 9.59 | 0.5478 |
| 75.0 | stepped | width | 61.8 | 74.7 | 79.7 | 4.3% | 12 | 184x448 7%, 184x358 23%, 184x298 27%, 184x224 43% | 9.59 | 0.5483 |
| 75.0 | predicted | width | 66.4 | 73.5 | 77.8 | 1.4% | 12 | 184x448 15%, 184x358 25%, 184x298 26%, 184x224 33% | 9.59 | 0.5485 |
| 75.0 | predicted | height | 64.6 | 73.2 | 77.2 | 1.7% | 10 | 368x358 2%, 368x298 14%, 368x224 36%, 184x224 48% | 9.57 | 0.5504 |

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
