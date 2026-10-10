# Dynamic resolution

A scene's camera can draw each frame at a render size picked to hold a frame
budget, instead of one fixed scale. The sizes are a ladder of steps, finest
first; `render_context_compose()` copies an exact-half draw into the half
picture when available and otherwise upscales to the panel size with
`render/upscale.h`, which takes any ratio in either axis. The code is
`launcher/main/render/resolution/`. It is opt-in, a setting of the render
context rather than of a scene or a camera: an app turns it on with
`render_context_set_dynamic_resolution()` ([Mesh-Rendering.md](Mesh-Rendering.md#the-render-context)),
and with no call every frame draws at the context's fixed scale.

| Policy | Chooses | From | Guards against flapping |
|---|---|---|---|
| Stepped controller | one step at a time, after the cost | the mean of a window of measured frames against an up and a down threshold | a cooldown after each step that doubles when a step reverses the last one; a panic drop for one frame far over budget |
| Predictor | any step, before the cost | a linear model of the frame, priced at every step from the triangles culling kept this frame, then scaled and offset by what recent frames cost | going finer needs a margin under the budget |

The predictor's model is fitted on the board along one camera path. Live
frames refit it: a two-state Kalman filter<sup>[[22]](../Citations.md#22)</sup> learns a scale and an offset on
the fitted price from each frame's measured cost, leans back to the plain fit
when frames stop disagreeing with it, and clips a single hitch frame. A scale
and an offset can be learnt from frames at any one step, which four separate
weights cannot, so the fit's shape across steps carries over to steps the
predictor has not drawn lately. The scene's second path, which the fit never
sees, measures how well the refit tracks a view the fit did not cover.

The cost a policy holds is the part that scales: the draw and the compose
each step pays. Calibration uses the render context's compose decision,
including the copy into the half picture at the exactly-half step.
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

<!-- generated: dynres-stages sha256=c1a852ddc1187aca789db5d83072d6fbaa38a66a8faa13d6ee3f73c732381996 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 110.4 | 111.5 | 126.3 | 0.6 | 4.2 | 95.1 | 10.3 | 31.9 | 34.2 | 35.0 | 47.1 |
| 184x448 | 2.00 x 1.00 | 50% | 83.3 | 84.4 | 96.2 | 0.6 | 4.2 | 71.1 | 7.2 | 30.0 | 32.8 | 30.6 | 18.3 |
| 184x358 | 2.00 x 1.25 | 40% | 70.8 | 71.8 | 83.8 | 0.6 | 4.3 | 59.2 | 6.6 | 29.1 | 27.0 | 24.4 | 12.6 |
| 184x298 | 2.00 x 1.50 | 33% | 63.1 | 63.4 | 75.0 | 0.6 | 4.2 | 51.9 | 6.3 | 28.5 | 23.0 | 20.2 | 9.5 |
| 184x224 | 2.00 | 25% | 53.6 | 54.0 | 65.1 | 0.6 | 4.3 | 42.8 | 5.8 | 27.6 | 18.1 | 15.1 | 6.2 |
| 184x179 | 2.00 x 2.50 | 20% | 47.8 | 48.1 | 58.3 | 0.7 | 4.3 | 37.3 | 5.5 | 26.8 | 15.1 | 12.0 | 3.8 |
| 184x149 | 2.00 x 3.01 | 17% | 43.7 | 44.3 | 53.4 | 0.6 | 4.3 | 33.4 | 5.4 | 26.2 | 13.1 | 10.0 | 2.5 |
| 368x358 | 1.00 x 1.25 | 80% | 93.1 | 94.0 | 109.3 | 0.6 | 4.2 | 78.8 | 9.3 | 31.2 | 28.2 | 27.9 | 35.8 |
| 368x298 | 1.00 x 1.50 | 67% | 81.3 | 82.1 | 96.1 | 0.6 | 4.2 | 68.2 | 8.2 | 30.7 | 24.2 | 23.1 | 28.4 |
| 368x224 | 1.00 x 2.00 | 50% | 67.5 | 68.1 | 81.3 | 0.6 | 4.2 | 55.3 | 7.3 | 29.9 | 19.2 | 17.3 | 19.6 |
| 294x358 | 1.25 | 64% | 86.4 | 87.3 | 101.8 | 0.6 | 4.2 | 71.2 | 10.3 | 30.5 | 27.8 | 26.8 | 26.0 |
| 276x336 | 1.33 | 56% | 81.6 | 82.5 | 96.1 | 0.6 | 4.2 | 66.3 | 10.3 | 30.1 | 26.2 | 24.8 | 22.7 |
| 245x298 | 1.50 | 44% | 72.1 | 72.5 | 85.7 | 0.6 | 4.2 | 57.6 | 9.6 | 29.3 | 23.5 | 21.5 | 15.5 |
| 210x256 | 1.75 | 33% | 64.4 | 64.9 | 76.2 | 0.6 | 4.2 | 50.1 | 9.4 | 28.4 | 20.4 | 17.9 | 11.3 |
| 245x224 | 1.50 x 2.00 | 33% | 61.7 | 62.1 | 74.3 | 0.6 | 4.2 | 47.3 | 9.5 | 28.4 | 18.6 | 16.1 | 10.7 |
| 147x179 | 2.50 | 16% | 48.2 | 48.6 | 57.9 | 0.7 | 4.3 | 34.7 | 8.6 | 26.0 | 14.7 | 11.4 | 1.8 |
| 122x149 | 3.02 | 11% | 43.0 | 42.7 | 51.7 | 0.6 | 4.3 | 29.6 | 8.4 | 24.8 | 12.4 | 8.9 | -0.2 |

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
floor no step goes under. The split times a full upscale at every size; a
live frame at exactly half copies into the half picture instead, and the
policies below are priced and flown with that copy.

<!-- generated: dynres-findings sha256=4bf00b5134e13304a7f20791e7c58fdbedab5ab8ddfec5b564ec8ffbefb7f418 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| half width, more height over half | 184x298 minus 184x224 | 9.5 | 9.0 | 0.5 | 4.9 | 5.1 | 3.3 |
| half over floor | 184x224 minus 184x179 | 5.8 | 5.6 | 0.3 | 3.0 | 3.1 | 2.4 |
| floor over 2.5x cost reference | 184x179 minus 147x179 | -0.4 | 2.6 | -3.0 | 0.4 | 0.7 | 2.0 |
| recovery over 3x cost reference | 184x149 minus 122x149 | 0.7 | 3.7 | -3.0 | 0.6 | 1.0 | 2.7 |
| full height over half | 368x448 minus 368x224 | 42.9 | 39.8 | 3.1 | 15.0 | 17.7 | 27.5 |
| full width over half | 368x448 minus 184x448 | 27.1 | 24.0 | 3.1 | 1.4 | 4.4 | 28.8 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew each camera's whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=2235467a47428ed08518721e8e95914a20155faaff845c6e41f82d7c98e82e49 -->
| Path | Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| camera | 60.0 | fixed | half | 50.5 | 60.7 | 62.9 | 10.5% | 0 | 184x224 100% | 9.53 | 0.5581 |
| camera | 60.0 | stepped | width | 51.8 | 59.8 | 64.0 | 4.5% | 9 | 184x358 3%, 184x298 25%, 184x224 43%, 184x179 29% | 9.54 | 0.5572 |
| camera | 60.0 | predicted | width | 53.8 | 58.8 | 60.7 | 1.1% | 12 | 184x448 2%, 184x358 10%, 184x298 17%, 184x224 56%, 184x179 15% | 9.53 | 0.5578 |
| camera | 60.0 | predicted | height | 51.9 | 58.7 | 62.5 | 1.1% | 8 | 368x298 2%, 368x224 12%, 184x224 71%, 184x179 15% | 9.53 | 0.5582 |
| camera | 75.0 | fixed | half | 50.5 | 60.7 | 62.9 | 0.0% | 0 | 184x224 100% | 9.53 | 0.5581 |
| camera | 75.0 | stepped | width | 63.7 | 75.1 | 91.4 | 5.5% | 12 | 368x448 1%, 184x448 11%, 184x358 27%, 184x298 45%, 184x224 16% | 9.52 | 0.5590 |
| camera | 75.0 | predicted | width | 67.0 | 73.3 | 76.0 | 0.7% | 10 | 184x448 17%, 184x358 38%, 184x298 31%, 184x224 14% | 9.52 | 0.5592 |
| camera | 75.0 | predicted | height | 62.7 | 70.8 | 75.5 | 0.6% | 10 | 368x358 3%, 368x298 17%, 368x224 43%, 184x224 37% | 9.50 | 0.5612 |
| tour | 60.0 | fixed | half | 29.7 | 48.3 | 51.7 | 0.0% | 0 | 184x224 100% | 12.05 | 0.4968 |
| tour | 60.0 | stepped | width | 45.3 | 71.2 | 90.8 | 16.8% | 20 | 368x448 7%, 184x448 24%, 184x358 36%, 184x298 29%, 184x224 4% | 12.04 | 0.4974 |
| tour | 60.0 | predicted | width | 50.4 | 58.0 | 61.9 | 0.8% | 23 | 184x448 64%, 184x358 18%, 184x298 10%, 184x224 8% | 12.04 | 0.4977 |
| tour | 60.0 | predicted | height | 51.7 | 58.8 | 61.0 | 1.2% | 25 | 368x448 2%, 368x358 17%, 368x298 43%, 368x224 23%, 184x224 15% | 12.04 | 0.4978 |
| tour | 75.0 | fixed | half | 29.7 | 48.3 | 51.7 | 0.0% | 0 | 184x224 100% | 12.05 | 0.4968 |
| tour | 75.0 | stepped | width | 62.6 | 77.6 | 89.3 | 9.3% | 23 | 368x448 37%, 184x448 43%, 184x358 18%, 184x298 1%, 184x224 1% | 12.04 | 0.4983 |
| tour | 75.0 | predicted | width | 60.1 | 72.0 | 76.6 | 0.8% | 12 | 368x448 23%, 184x448 64%, 184x358 13% | 12.04 | 0.4982 |
| tour | 75.0 | predicted | height | 66.5 | 72.7 | 76.4 | 1.0% | 21 | 368x448 29%, 368x358 44%, 368x298 16%, 368x224 12% | 12.03 | 0.4992 |

Frame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM are against the reference at full size, lower dE and higher SSIM being closer.
<!-- /generated: dynres-policies -->

Prediction error compares each predicted frame's corrected price with its
measured cost, grouped by path, ladder and budget.

<!-- generated: dynres-prediction sha256=d69b570e53337f8f68e53e666f7cc4ec60ac8f4d8f409dca8818537f7b0fe457 -->
| Path | Ladder | Budget | Frames | Median error | p95 error | Over budget |
|---|---|---|---|---|---|---|
| camera | height | 60.0 | 727 | 0.5% | 2.4% | 1.1% |
| camera | width | 60.0 | 727 | 0.4% | 2.4% | 1.1% |
| camera | height | 75.0 | 727 | 0.4% | 1.9% | 0.6% |
| camera | width | 75.0 | 727 | 0.4% | 2.2% | 0.7% |
| tour | height | 60.0 | 900 | 0.6% | 2.4% | 1.2% |
| tour | width | 60.0 | 900 | 0.6% | 2.6% | 0.8% |
| tour | height | 75.0 | 900 | 0.6% | 2.4% | 1.0% |
| tour | width | 75.0 | 900 | 0.6% | 2.5% | 0.8% |

Online refit: 1000 calls, mean 3.636 us, max 30 us per call.
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
autana suite run_raster_scale_perf_suite --flash --out docs/render/data/dynamic-resolution-board.log
sh launcher/tools/render/render_doc_images.sh
```

For the pipeline captures and GPU-stage tables, see
[Render-Pipeline.md](Render-Pipeline.md#refreshing).
