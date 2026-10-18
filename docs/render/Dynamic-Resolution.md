# Dynamic resolution

A scene's camera can draw each frame at a render size picked to hold a frame
budget, instead of one fixed scale. The sizes are a ladder of steps, finest
first.

`render_context_compose()` expands a draw into the half picture when its width is
exactly half and its height divides the half height. Otherwise it upscales to the
panel size with `render/upscale.h`, which takes any ratio in either axis.

The code is `launcher/main/render/resolution/`. Resolution selection is a
setting of the render context rather than of a scene or a camera.

An app opts in with `render_context_set_dynamic_resolution()`
([Mesh-Rendering.md](Mesh-Rendering.md#the-render-context)). With no call every
frame draws at the context's fixed scale.

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
including expansion into the half picture for compatible steps.
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

<!-- generated: dynres-stages sha256=e0c8ed5500c2d7cc59aca851d3ba8b17987c7a01e5e93a673c2497cf9698fe27 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | edges | carry | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 94.1 | 92.8 | 113.2 | 0.6 | 3.0 | 80.1 | 10.3 | 29.0 | 10.9 | 26.5 | 59.2 |
| 184x448 | 2.00 x 1.00 | 50% | 69.2 | 69.2 | 82.2 | 0.7 | 3.1 | 58.1 | 7.3 | 27.0 | 10.1 | 25.5 | 31.6 |
| 184x358 | 2.00 x 1.25 | 40% | 59.0 | 59.4 | 70.8 | 0.7 | 3.1 | 48.6 | 6.6 | 26.2 | 9.1 | 20.8 | 23.3 |
| 184x298 | 2.00 x 1.50 | 33% | 52.8 | 53.6 | 64.0 | 0.7 | 3.1 | 42.8 | 6.3 | 25.7 | 8.3 | 17.6 | 18.3 |
| 184x224 | 2.00 | 25% | 45.4 | 46.4 | 57.2 | 0.7 | 3.1 | 35.9 | 5.8 | 24.8 | 7.3 | 13.7 | 12.8 |
| 184x179 | 2.00 x 2.50 | 20% | 40.5 | 40.7 | 49.9 | 0.7 | 3.1 | 31.2 | 5.5 | 24.1 | 6.7 | 11.2 | 9.1 |
| 184x149 | 2.00 x 3.01 | 17% | 37.6 | 38.9 | 47.4 | 0.7 | 3.1 | 28.4 | 5.4 | 23.5 | 6.2 | 9.6 | 6.9 |
| 368x358 | 1.00 x 1.25 | 80% | 79.2 | 78.7 | 95.6 | 0.7 | 3.0 | 66.3 | 9.2 | 28.3 | 9.9 | 21.7 | 45.3 |
| 368x298 | 1.00 x 1.50 | 67% | 69.5 | 68.6 | 84.4 | 0.7 | 3.1 | 57.6 | 8.2 | 27.8 | 9.1 | 18.5 | 36.2 |
| 368x224 | 1.00 x 2.00 | 50% | 58.0 | 57.9 | 71.3 | 0.6 | 3.1 | 47.1 | 7.2 | 27.1 | 8.2 | 14.5 | 25.3 |
| 294x358 | 1.25 | 64% | 72.7 | 72.5 | 87.2 | 0.7 | 3.0 | 59.6 | 9.4 | 27.5 | 9.6 | 21.4 | 36.4 |
| 276x336 | 1.33 | 56% | 68.2 | 68.3 | 82.2 | 0.7 | 3.1 | 55.2 | 9.2 | 27.2 | 9.2 | 20.2 | 32.3 |
| 245x298 | 1.50 | 44% | 60.3 | 60.5 | 73.1 | 0.7 | 3.1 | 48.0 | 8.5 | 26.5 | 8.7 | 18.0 | 24.3 |
| 210x256 | 1.75 | 33% | 54.1 | 55.3 | 64.8 | 0.6 | 3.1 | 42.2 | 8.2 | 25.6 | 7.9 | 15.6 | 18.9 |
| 245x224 | 1.50 x 2.00 | 33% | 51.9 | 52.6 | 63.4 | 0.7 | 3.1 | 39.9 | 8.3 | 25.7 | 7.7 | 14.0 | 17.1 |
| 147x179 | 2.50 | 16% | 40.1 | 40.2 | 49.6 | 0.7 | 3.1 | 29.0 | 7.3 | 23.4 | 6.4 | 11.0 | 7.1 |
| 122x149 | 3.02 | 11% | 35.8 | 36.6 | 43.8 | 0.7 | 3.1 | 24.9 | 7.2 | 22.1 | 5.7 | 9.1 | 4.2 |

Milliseconds; both cores unless marked one core.
<!-- /generated: dynres-stages -->

The questions the split answers, as differences between two sizes. A size
that keeps the panel's width or halves it upscales whole rows or pixel pairs;
any other width pays the mapped upscale on top, so both ladders keep to those
two widths. Halving the height saves far more than halving the width, since
edges and attribute carry follow the height, so a ladder cuts the height first. The
floor and the recovery step are named for what they cost, not their scale:
184x179 costs about what isotropic 2.5x (147x179) does, and 184x149 what 3x
(122x149) does, the upscale they save paying for the pixels they keep. The one-core setup stage barely moves with size: it is the
floor no step goes under. The split times a full upscale at every size; a
live frame at exactly half copies into the half picture instead, and the
policies below are priced and flown with that copy.

<!-- generated: dynres-findings sha256=323030827c7446bfe5757d538ab4748a72001bb09d997591aa81caf7aa27d876 -->
| Question | Compared | Frame | draw | upscale | 1 core: edges | carry | fill |
|---|---|---|---|---|---|---|---|
| half width, more height over half | 184x298 minus 184x224 | 7.4 | 6.9 | 0.5 | 1.0 | 3.9 | 5.6 |
| half over floor | 184x224 minus 184x179 | 4.9 | 4.7 | 0.3 | 0.7 | 2.4 | 3.6 |
| floor over 2.5x cost reference | 184x179 minus 147x179 | 0.4 | 2.2 | -1.8 | 0.3 | 0.3 | 2.1 |
| recovery over 3x cost reference | 184x149 minus 122x149 | 1.7 | 3.5 | -1.8 | 0.5 | 0.5 | 2.7 |
| full height over half | 368x448 minus 368x224 | 36.1 | 33.1 | 3.1 | 2.7 | 12.1 | 33.9 |
| full width over half | 368x448 minus 184x448 | 24.9 | 22.0 | 3.0 | 0.8 | 1.0 | 27.6 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew each camera's whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=3c509205db4b36d7c8bf200a21e925368b99f8414ae2238b2dc9d3679fb76667 -->
| Path | Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|---|
| camera | 60.0 | fixed | half | 42.5 | 53.6 | 54.7 | 0.0% | 0 | 184x224 100% | 9.53 | 0.5581 |
| camera | 60.0 | stepped | width | 50.2 | 60.1 | 65.2 | 5.6% | 10 | 184x448 3%, 184x358 25%, 184x298 38%, 184x224 34% | 9.53 | 0.5584 |
| camera | 60.0 | predicted | width | 53.0 | 58.2 | 60.8 | 0.8% | 12 | 184x448 17%, 184x358 24%, 184x298 26%, 184x224 34% | 9.52 | 0.5587 |
| camera | 60.0 | predicted | height | 51.2 | 58.9 | 60.9 | 0.4% | 10 | 368x358 2%, 368x298 12%, 368x224 36%, 184x224 50% | 9.51 | 0.5604 |
| camera | 75.0 | fixed | half | 42.5 | 53.6 | 54.7 | 0.0% | 0 | 184x224 100% | 9.53 | 0.5581 |
| camera | 75.0 | stepped | width | 62.4 | 88.9 | 93.0 | 25.0% | 12 | 368x448 14%, 184x448 43%, 184x358 28%, 184x298 14%, 184x224 2% | 9.50 | 0.5608 |
| camera | 75.0 | predicted | width | 65.8 | 72.0 | 76.2 | 0.4% | 7 | 368x448 2%, 184x448 61%, 184x358 30%, 184x298 7% | 9.51 | 0.5601 |
| camera | 75.0 | predicted | height | 66.6 | 72.8 | 76.3 | 0.4% | 11 | 368x448 2%, 368x358 25%, 368x298 31%, 368x224 41%, 184x224 0% | 9.46 | 0.5640 |
| tour | 60.0 | fixed | half | 26.1 | 40.8 | 44.2 | 0.0% | 0 | 184x224 100% | 12.05 | 0.4968 |
| tour | 60.0 | stepped | width | 47.4 | 67.9 | 92.1 | 15.9% | 15 | 368x448 10%, 184x448 57%, 184x358 31%, 184x298 1%, 184x224 1% | 12.04 | 0.4981 |
| tour | 60.0 | predicted | width | 48.5 | 57.7 | 61.8 | 1.6% | 14 | 368x448 4%, 184x448 77%, 184x358 15%, 184x298 4% | 12.04 | 0.4981 |
| tour | 60.0 | predicted | height | 52.8 | 58.5 | 61.6 | 0.9% | 19 | 368x448 4%, 368x358 42%, 368x298 32%, 368x224 19%, 184x224 2% | 12.03 | 0.4987 |
| tour | 75.0 | fixed | half | 26.1 | 40.8 | 44.2 | 0.0% | 0 | 184x224 100% | 12.05 | 0.4968 |
| tour | 75.0 | stepped | width | 65.8 | 74.5 | 81.1 | 4.3% | 14 | 368x448 66%, 184x448 32%, 184x358 1%, 184x298 1%, 184x224 1% | 12.03 | 0.4989 |
| tour | 75.0 | predicted | width | 63.1 | 73.0 | 76.7 | 0.8% | 9 | 368x448 50%, 184x448 50% | 12.03 | 0.4988 |
| tour | 75.0 | predicted | height | 66.0 | 73.3 | 76.7 | 0.6% | 14 | 368x448 63%, 368x358 25%, 368x298 12% | 12.02 | 0.4997 |

Frame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM are against the reference at full size, lower dE and higher SSIM being closer.
<!-- /generated: dynres-policies -->

Prediction error compares each predicted frame's corrected price with its
measured cost, grouped by path, ladder and budget.

<!-- generated: dynres-prediction sha256=d1879a5ff699a7511f4d77f8afb5764e679c3329cdb81f515bf562ced67794a6 -->
| Path | Ladder | Budget | Frames | Median error | p95 error | Over budget |
|---|---|---|---|---|---|---|
| camera | height | 60.0 | 727 | 0.5% | 2.4% | 0.4% |
| camera | width | 60.0 | 727 | 0.5% | 2.2% | 0.8% |
| camera | height | 75.0 | 727 | 0.4% | 2.1% | 0.4% |
| camera | width | 75.0 | 727 | 0.4% | 2.3% | 0.4% |
| tour | height | 60.0 | 900 | 0.5% | 2.1% | 0.9% |
| tour | width | 60.0 | 900 | 0.7% | 2.4% | 1.6% |
| tour | height | 75.0 | 900 | 0.6% | 2.2% | 0.6% |
| tour | width | 75.0 | 900 | 0.6% | 2.4% | 0.8% |

Online refit: 1000 calls, mean 3.730 us, max 27 us per call.
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

## Height-only scale and experimental scanline present

`render_context_set_scale(context, width_percent, height_percent)` sets the
fixed shares independently. A 50% width and 25% height draws 184×112 on a
368×448 destination.

`raster_fits_half_picture()` accepts exactly half width when the half height
is a whole multiple of the drawn height. Compose upscales to the half picture;
present doubles it in both axes. Other sizes compose to the full destination.

The scanline experiment is off by default. `render_context_set_scanlines()`
darkens the second half of each repeated-row run when every source row has
the same even run length. Odd and uneven runs retain their pixels.

The rule uses the actual row map for paired copies only; other paths ignore
the strength. `upscale_rows()` copies unchanged from IRAM. The raster selects
`upscale_rows_scanlined()` in flash for nonzero strength; it falls back to
`upscale_rows()` when the run is ineligible.
See `UPSCALE_SCANLINE_UNIT` for strength units: zero preserves colour,
half the unit halves each RGB565 channel, and `UPSCALE_SCANLINE_MAX` gives black.

Depth-empty pixels use the clear colour before darkening. A doubled row in
the half picture becomes two bright and two dark panel rows.
The runs follow panel rows, so a rotated display shows them as columns.

The caller's tools README owns the removal inventory and its check
(`git grep -n -i scanline ...`), host commands and visual comparisons.

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
