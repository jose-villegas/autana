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

The cost a policy holds is the part that scales: the draw (with the
predictor's cull-only census) and the upscale. The present and whatever an
app draws over the scene are not in it.

## Where a frame's time goes at each size

The raster brackets its stages for [frame cost](../tools/Frame-Cost.md):
`r3d.cull`, `r3d.transform`, `r3d.draw` and `r3d.upscale`, and the
predictor's census as `r3d.census`. The suite behind
these tables draws the test scene's path at every size, both cores, and then
once more on one core with the span rasterizer stopped after each stage.

<!-- generated: dynres-stages sha256=edcdff3b8fb72a80a1baf3a3b9195009cf186f559c960ac5b589a33274a83285 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 110.1 | 111.3 | 129.8 | 0.6 | 4.4 | 91.7 | 13.3 | 32.9 | 33.5 | 33.4 | 44.8 |
| 294x358 | 1.25 | 64% | 85.0 | 85.6 | 103.1 | 0.7 | 4.4 | 69.5 | 10.5 | 31.3 | 27.3 | 25.5 | 25.7 |
| 276x336 | 1.33 | 56% | 80.5 | 81.0 | 98.2 | 0.7 | 4.4 | 64.8 | 10.7 | 30.9 | 25.7 | 23.6 | 22.5 |
| 245x298 | 1.50 | 44% | 71.8 | 71.7 | 88.1 | 0.6 | 4.4 | 56.9 | 9.9 | 30.1 | 23.1 | 20.4 | 16.1 |
| 210x256 | 1.75 | 33% | 64.7 | 65.8 | 79.2 | 0.6 | 4.4 | 50.0 | 9.6 | 29.1 | 20.0 | 16.8 | 12.4 |
| 184x224 | 2.00 | 25% | 53.3 | 54.0 | 65.9 | 0.6 | 4.4 | 42.4 | 5.9 | 28.1 | 17.7 | 14.2 | 7.3 |
| 147x179 | 2.50 | 16% | 48.7 | 49.2 | 60.1 | 0.7 | 4.4 | 34.6 | 8.9 | 26.5 | 14.4 | 10.6 | 3.2 |
| 122x149 | 3.02 | 11% | 43.7 | 44.5 | 53.7 | 0.7 | 4.4 | 29.8 | 8.8 | 25.2 | 12.1 | 8.2 | 1.3 |
| 368x224 | 1.00 x 2.00 | 50% | 69.8 | 69.9 | 85.0 | 0.7 | 4.4 | 54.0 | 10.8 | 30.7 | 18.9 | 16.6 | 19.0 |
| 184x448 | 2.00 x 1.00 | 50% | 84.9 | 85.6 | 100.1 | 0.6 | 4.4 | 69.5 | 10.4 | 30.8 | 32.0 | 28.8 | 19.6 |
| 245x224 | 1.50 x 2.00 | 33% | 61.5 | 61.8 | 75.7 | 0.6 | 4.4 | 46.7 | 9.8 | 29.1 | 18.2 | 15.3 | 11.3 |
| 184x298 | 2.00 x 1.50 | 33% | 65.7 | 66.4 | 80.1 | 0.7 | 4.4 | 51.3 | 9.3 | 29.2 | 22.5 | 19.0 | 10.7 |

Milliseconds; both cores unless marked one core.
<!-- /generated: dynres-stages -->

The questions the split answers, as differences between two sizes. Most of
the gap between neighbouring isotropic steps is the raster, not the upscale;
a non-integer step pays the mapped upscale on top, which is why 2.5x saves
little over 2x. Halving the height saves far more than halving the width,
since rows and span setup follow the height, so a ladder cuts the height
first. The one-core setup stage barely moves with size: it is the floor no
step goes under.

<!-- generated: dynres-findings sha256=91cbe4930693c16f68d10cd6f9383ff6070a3a04895bb99a14d6c2b7eb020987 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| 1.5x over 2x | 245x298 minus 184x224 | 18.4 | 14.5 | 4.0 | 5.3 | 6.2 | 8.8 |
| 2x over 2.5x | 184x224 minus 147x179 | 4.7 | 7.8 | -3.0 | 3.3 | 3.6 | 4.1 |
| full height over half | 368x448 minus 368x224 | 40.2 | 37.7 | 2.6 | 14.6 | 16.9 | 25.8 |
| full width over half | 368x448 minus 184x448 | 25.2 | 22.2 | 3.0 | 1.6 | 4.7 | 25.2 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew the whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=d233fbd0c06b7eba04291baf99f9c73eb6c00782fa6c48e85fe9b0bcd0c1d3bf -->
| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|
| 60.0 | fixed | half | 54.0 | 65.7 | 67.8 | 23.7% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 60.0 | stepped | isotropic | 51.9 | 60.6 | 66.1 | 9.2% | 11 | 245x298 3%, 210x256 18%, 184x224 27%, 147x179 52% | 9.38 | 0.5504 |
| 60.0 | predicted | isotropic | 54.7 | 60.6 | 62.7 | 8.1% | 12 | 245x298 11%, 210x256 18%, 184x224 33%, 147x179 38% | 9.37 | 0.5515 |
| 60.0 | predicted | height | 54.2 | 60.8 | 64.1 | 8.0% | 14 | 368x298 2%, 368x224 11%, 245x224 20%, 184x224 29%, 147x179 38% | 9.35 | 0.5526 |
| 75.0 | fixed | half | 54.0 | 65.7 | 67.8 | 0.0% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 75.0 | stepped | isotropic | 64.9 | 74.5 | 82.9 | 4.3% | 12 | 368x448 1%, 294x358 11%, 245x298 20%, 210x256 40%, 184x224 28% | 9.30 | 0.5565 |
| 75.0 | predicted | isotropic | 66.3 | 73.2 | 77.1 | 1.7% | 10 | 294x358 16%, 245x298 26%, 210x256 38%, 184x224 20% | 9.30 | 0.5566 |
| 75.0 | predicted | height | 65.8 | 73.9 | 79.6 | 3.2% | 12 | 368x358 3%, 368x298 16%, 368x224 31%, 245x224 35%, 184x224 15% | 9.27 | 0.5590 |

Frame time is the scaled part, draw plus upscale, in milliseconds; dE and SSIM are against the reference at full size, lower dE and higher SSIM being closer.
<!-- /generated: dynres-policies -->

What the table shows:

- **Over budget.** At the tight budget the half scale itself is often over,
  and both policies step down to the 2.5x floor and stop there: a frame a
  little over budget costs less than a harsh drop in picture. The predictor
  steps on the frame the load arrives, the controller a window later.
- **Switches.** Neither flaps: the cooldown spaces the controller's steps,
  and the finer-step margin keeps the predictor from returning to a step it
  just left.
- **Quality.** The scorer's dE is mostly the bake against the reference, a
  floor every size shares, so the differences are small; the order is what
  counts. A full-width, half-height render scores closer to the reference
  than the isotropic step that costs as much, so the height-first ladder buys
  more picture for the same milliseconds.
- **Recovery.** 3x is reached only past the panic share; neither budget
  here comes near it. Below 2.5x a step buys little, as the setup floor in
  the stage table shows.

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
