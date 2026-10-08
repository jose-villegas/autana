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
| Predictor | any step, before the cost | a linear model of the frame, priced at every step from the triangles culling kept this frame, then scaled and offset by what recent frames cost | going finer needs a margin under the budget |

The predictor's model is fitted on the board along one camera path. Live
frames refit it: a two-state Kalman filter learns a scale and an offset on
the fitted price from each frame's measured cost, leans back to the plain fit
when frames stop disagreeing with it, and clips a single hitch frame. A scale
and an offset can be learnt from frames at any one step, which four separate
weights cannot, so the fit's shape across steps carries over to steps the
predictor has not drawn lately. The scene's second path, which the fit never
sees, measures how well the refit tracks a view the fit did not cover.

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

<!-- generated: dynres-stages sha256=0f4becbd9f8299effe743049c8208a8c665eb4eaf38421f367ce8c088cc55611 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 111.0 | 111.9 | 126.9 | 0.7 | 4.2 | 95.5 | 10.6 | 32.5 | 34.2 | 34.9 | 47.2 |
| 184x448 | 2.00 x 1.00 | 50% | 83.7 | 84.9 | 96.7 | 0.7 | 4.2 | 71.5 | 7.3 | 30.6 | 32.9 | 30.6 | 18.3 |
| 184x358 | 2.00 x 1.25 | 40% | 71.2 | 72.4 | 84.3 | 0.7 | 4.2 | 59.6 | 6.7 | 29.6 | 27.0 | 24.3 | 12.7 |
| 184x298 | 2.00 x 1.50 | 33% | 63.4 | 63.9 | 75.4 | 0.7 | 4.2 | 52.2 | 6.3 | 29.0 | 23.0 | 20.2 | 9.5 |
| 184x224 | 2.00 | 25% | 54.0 | 54.4 | 65.5 | 0.7 | 4.2 | 43.2 | 5.8 | 28.0 | 18.1 | 15.1 | 6.2 |
| 184x179 | 2.00 x 2.50 | 20% | 48.2 | 48.5 | 58.7 | 0.7 | 4.2 | 37.6 | 5.6 | 27.3 | 15.1 | 12.0 | 3.8 |
| 184x149 | 2.00 x 3.01 | 17% | 44.1 | 44.7 | 53.9 | 0.7 | 4.2 | 33.7 | 5.4 | 26.6 | 13.1 | 9.9 | 2.5 |
| 368x358 | 1.00 x 1.25 | 80% | 93.7 | 94.8 | 109.7 | 0.7 | 4.2 | 79.3 | 9.5 | 31.8 | 28.2 | 27.8 | 35.8 |
| 368x298 | 1.00 x 1.50 | 67% | 81.9 | 82.7 | 96.7 | 0.7 | 4.2 | 68.6 | 8.3 | 31.3 | 24.2 | 23.2 | 28.4 |
| 368x224 | 1.00 x 2.00 | 50% | 68.0 | 68.7 | 81.8 | 0.7 | 4.2 | 55.7 | 7.3 | 30.5 | 19.2 | 17.3 | 19.7 |
| 294x358 | 1.25 | 64% | 86.2 | 87.1 | 101.8 | 0.7 | 4.2 | 71.5 | 9.7 | 31.0 | 27.9 | 26.8 | 26.0 |
| 276x336 | 1.33 | 56% | 81.3 | 82.3 | 96.0 | 0.7 | 4.2 | 66.7 | 9.7 | 30.6 | 26.3 | 24.9 | 22.7 |
| 245x298 | 1.50 | 44% | 71.8 | 72.4 | 85.5 | 0.7 | 4.2 | 57.9 | 8.9 | 29.8 | 23.5 | 21.5 | 15.5 |
| 210x256 | 1.75 | 33% | 64.0 | 64.5 | 76.0 | 0.7 | 4.2 | 50.4 | 8.6 | 28.9 | 20.5 | 17.8 | 11.4 |
| 245x224 | 1.50 x 2.00 | 33% | 61.3 | 61.8 | 73.9 | 0.7 | 4.2 | 47.6 | 8.7 | 28.9 | 18.6 | 16.1 | 10.6 |
| 147x179 | 2.50 | 16% | 47.7 | 48.0 | 57.4 | 0.7 | 4.2 | 35.0 | 7.7 | 26.5 | 14.7 | 11.4 | 1.8 |
| 122x149 | 3.02 | 11% | 42.5 | 42.1 | 51.2 | 0.7 | 4.2 | 30.0 | 7.5 | 25.2 | 12.4 | 8.9 | -0.2 |

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

<!-- generated: dynres-findings sha256=10fc0a496df624ee9ce20a7b0dc62d334a2a2e52c75c702a83ac50a0addcb0a3 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| half width, more height over half | 184x298 minus 184x224 | 9.5 | 9.0 | 0.5 | 4.9 | 5.1 | 3.3 |
| half over floor | 184x224 minus 184x179 | 5.8 | 5.6 | 0.3 | 3.0 | 3.1 | 2.3 |
| floor over 2.5x cost reference | 184x179 minus 147x179 | 0.5 | 2.6 | -2.2 | 0.4 | 0.6 | 2.1 |
| recovery over 3x cost reference | 184x149 minus 122x149 | 1.6 | 3.7 | -2.1 | 0.6 | 1.0 | 2.7 |
| full height over half | 368x448 minus 368x224 | 43.0 | 39.8 | 3.2 | 15.0 | 17.6 | 27.6 |
| full width over half | 368x448 minus 184x448 | 27.3 | 24.0 | 3.3 | 1.4 | 4.4 | 28.9 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew each camera's whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=a5626658a0f08efc3762eec0ad984a4af77add5290f00093110e0bf1f69d7aa0 -->
| Path | Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| camera | 60.0 | fixed | half | 54.0 | 64.2 | 66.4 | 21.9% | 0 | 184x224 100% | 9.53 | 0.5581 |
| camera | 60.0 | stepped | width | 51.7 | 60.0 | 64.2 | 5.0% | 11 | 184x358 3%, 184x298 17%, 184x224 29%, 184x179 52% | 9.55 | 0.5566 |
| camera | 60.0 | predicted | width | 54.5 | 59.0 | 60.8 | 1.1% | 14 | 184x448 2%, 184x358 10%, 184x298 16%, 184x224 39%, 184x179 33% | 9.54 | 0.5572 |
| camera | 60.0 | predicted | height | 53.6 | 58.8 | 62.9 | 0.7% | 10 | 368x298 2%, 368x224 11%, 184x224 55%, 184x179 32% | 9.53 | 0.5577 |
| camera | 75.0 | fixed | half | 54.0 | 64.2 | 66.4 | 0.0% | 0 | 184x224 100% | 9.53 | 0.5581 |
| camera | 75.0 | stepped | width | 64.1 | 75.4 | 93.5 | 5.4% | 12 | 368x448 1%, 184x448 11%, 184x358 25%, 184x298 42%, 184x224 21% | 9.52 | 0.5589 |
| camera | 75.0 | predicted | width | 67.1 | 73.5 | 76.1 | 1.0% | 10 | 184x448 17%, 184x358 37%, 184x298 31%, 184x224 14% | 9.52 | 0.5592 |
| camera | 75.0 | predicted | height | 64.2 | 71.0 | 75.9 | 0.6% | 10 | 368x358 3%, 368x298 16%, 368x224 43%, 184x224 38% | 9.50 | 0.5611 |
| tour | 60.0 | fixed | half | 33.2 | 51.9 | 55.4 | 0.0% | 0 | 184x224 100% | 12.05 | 0.4968 |
| tour | 60.0 | stepped | width | 45.5 | 71.7 | 91.3 | 17.2% | 20 | 368x448 7%, 184x448 23%, 184x358 36%, 184x298 29%, 184x224 4% | 12.04 | 0.4974 |
| tour | 60.0 | predicted | width | 51.2 | 58.2 | 62.4 | 1.1% | 23 | 184x448 64%, 184x358 18%, 184x298 9%, 184x224 9% | 12.04 | 0.4977 |
| tour | 60.0 | predicted | height | 51.9 | 58.8 | 61.4 | 1.0% | 21 | 368x358 16%, 368x298 43%, 368x224 24%, 184x224 16% | 12.04 | 0.4977 |
| tour | 75.0 | fixed | half | 33.2 | 51.9 | 55.4 | 0.0% | 0 | 184x224 100% | 12.05 | 0.4968 |
| tour | 75.0 | stepped | width | 66.3 | 78.8 | 85.2 | 10.8% | 22 | 368x448 40%, 184x448 43%, 184x358 15%, 184x298 1%, 184x224 1% | 12.04 | 0.4984 |
| tour | 75.0 | predicted | width | 60.0 | 72.5 | 76.5 | 0.7% | 12 | 368x448 22%, 184x448 65%, 184x358 13% | 12.04 | 0.4982 |
| tour | 75.0 | predicted | height | 66.6 | 72.7 | 76.4 | 1.4% | 23 | 368x448 28%, 368x358 44%, 368x298 15%, 368x224 13% | 12.03 | 0.4991 |

Frame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM are against the reference at full size, lower dE and higher SSIM being closer.
<!-- /generated: dynres-policies -->

Prediction error compares each predicted frame's corrected price with its
measured draw plus upscale cost, grouped by path, ladder and budget.

<!-- generated: dynres-prediction sha256=70452cc80bb8df0b1ea67b8db98cbd77728f7604608262340e146f9ddc4ec65d -->
| Path | Ladder | Budget | Frames | Median error | p95 error | Over budget |
|---|---|---|---|---|---|---|
| camera | height | 60.0 | 727 | 0.4% | 2.4% | 0.7% |
| camera | width | 60.0 | 727 | 0.4% | 2.2% | 1.1% |
| camera | height | 75.0 | 727 | 0.4% | 2.0% | 0.6% |
| camera | width | 75.0 | 727 | 0.4% | 2.2% | 1.0% |
| tour | height | 60.0 | 900 | 0.6% | 2.4% | 1.0% |
| tour | width | 60.0 | 900 | 0.6% | 2.6% | 1.1% |
| tour | height | 75.0 | 900 | 0.6% | 2.4% | 1.4% |
| tour | width | 75.0 | 900 | 0.6% | 2.5% | 0.7% |

Online refit: 1000 calls, mean 4.319 us, max 33 us per call.
<!-- /generated: dynres-prediction -->

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
from the board capture and per-path quality CSVs in `data/`:
`dynamic-resolution-quality-camera.csv` and
`dynamic-resolution-quality-tour.csv`.
The capture is the suite's `scale_split`, `scale_spans`, `dynres_step` and
`dynres_frames` lines; each CSV is made by the test scene's quality script, which
renders the path at each size on a host and scores it. A new board run
replaces the capture, and the generator does the rest:

```sh
autana suite run_raster_scale_perf_suite --flash
sh launcher/tools/render/render_doc_images.sh
```
