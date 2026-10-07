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

<!-- generated: dynres-stages sha256=8dadb99e9bca61808fbbac2281e9730d20a8efa1e099430db8ec2702ae51d462 -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 111.6 | 112.4 | 131.5 | 0.6 | 4.3 | 93.6 | 13.0 | 33.5 | 33.4 | 33.4 | 48.7 |
| 294x358 | 1.25 | 64% | 86.1 | 86.4 | 104.5 | 0.6 | 4.4 | 70.8 | 10.2 | 31.9 | 27.2 | 25.6 | 28.2 |
| 276x336 | 1.33 | 56% | 81.4 | 81.5 | 99.2 | 0.6 | 4.4 | 66.0 | 10.3 | 31.5 | 25.6 | 23.6 | 24.8 |
| 245x298 | 1.50 | 44% | 72.5 | 72.4 | 88.9 | 0.7 | 4.4 | 57.9 | 9.6 | 30.6 | 23.0 | 20.4 | 17.9 |
| 210x256 | 1.75 | 33% | 65.2 | 66.0 | 79.9 | 0.6 | 4.4 | 50.8 | 9.3 | 29.6 | 19.9 | 16.8 | 13.7 |
| 184x224 | 2.00 | 25% | 54.0 | 54.6 | 66.7 | 0.6 | 4.4 | 43.1 | 5.8 | 28.6 | 17.6 | 14.2 | 8.3 |
| 147x179 | 2.50 | 16% | 48.8 | 49.4 | 60.2 | 0.7 | 4.4 | 35.1 | 8.6 | 27.0 | 14.3 | 10.6 | 3.8 |
| 122x149 | 3.02 | 11% | 43.7 | 44.4 | 53.7 | 0.7 | 4.4 | 30.2 | 8.4 | 25.6 | 12.0 | 8.2 | 1.7 |
| 368x224 | 1.00 x 2.00 | 50% | 70.6 | 70.5 | 86.0 | 0.6 | 4.4 | 55.1 | 10.4 | 31.2 | 18.8 | 16.6 | 20.9 |
| 184x448 | 2.00 x 1.00 | 50% | 85.8 | 86.3 | 101.1 | 0.6 | 4.4 | 70.7 | 10.0 | 31.3 | 31.9 | 28.7 | 21.8 |
| 245x224 | 1.50 x 2.00 | 33% | 62.0 | 62.2 | 76.2 | 0.6 | 4.4 | 47.5 | 9.4 | 29.6 | 18.1 | 15.2 | 12.7 |
| 184x298 | 2.00 x 1.50 | 33% | 66.3 | 67.0 | 80.8 | 0.6 | 4.4 | 52.2 | 9.1 | 29.7 | 22.4 | 19.0 | 12.1 |

Milliseconds; both cores unless marked one core.
<!-- /generated: dynres-stages -->

The questions the split answers, as differences between two sizes. Most of
the gap between neighbouring isotropic steps is the raster, not the upscale;
a non-integer step pays the mapped upscale on top, which is why 2.5x saves
little over 2x. Halving the height saves far more than halving the width,
since rows and span setup follow the height, so a ladder cuts the height
first. The one-core setup stage barely moves with size: it is the floor no
step goes under.

<!-- generated: dynres-findings sha256=e3fd2e7e30eaec3845fb80a8ae2b647d7e442ec72404ce5674ecc1f5addcaf32 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| 1.5x over 2x | 245x298 minus 184x224 | 18.5 | 14.8 | 3.7 | 5.3 | 6.2 | 9.6 |
| 2x over 2.5x | 184x224 minus 147x179 | 5.2 | 7.9 | -2.7 | 3.3 | 3.6 | 4.5 |
| full height over half | 368x448 minus 368x224 | 41.0 | 38.4 | 2.7 | 14.5 | 16.9 | 27.8 |
| full width over half | 368x448 minus 184x448 | 25.8 | 22.8 | 3.0 | 1.5 | 4.7 | 26.9 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew the whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=49547fadc5e0fa3c54abb163704665e15e4dca2ba83c1d0db06e2a39baee6d2c -->
| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|
| 60.0 | fixed | half | 54.7 | 66.4 | 68.5 | 24.8% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 60.0 | stepped | isotropic | 52.0 | 60.7 | 66.8 | 10.2% | 11 | 245x298 3%, 210x256 17%, 184x224 27%, 147x179 53% | 9.38 | 0.5503 |
| 60.0 | predicted | isotropic | 54.6 | 60.8 | 63.3 | 9.2% | 12 | 245x298 7%, 210x256 21%, 184x224 32%, 147x179 41% | 9.37 | 0.5511 |
| 60.0 | predicted | height | 54.3 | 60.8 | 63.8 | 8.7% | 14 | 368x298 2%, 368x224 10%, 245x224 20%, 184x224 28%, 147x179 40% | 9.36 | 0.5522 |
| 75.0 | fixed | half | 54.7 | 66.4 | 68.5 | 0.0% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 75.0 | stepped | isotropic | 65.6 | 74.6 | 83.4 | 4.1% | 12 | 368x448 1%, 294x358 11%, 245x298 19%, 210x256 41%, 184x224 28% | 9.30 | 0.5564 |
| 75.0 | predicted | isotropic | 66.9 | 73.7 | 78.4 | 1.7% | 10 | 294x358 14%, 245x298 26%, 210x256 39%, 184x224 20% | 9.30 | 0.5566 |
| 75.0 | predicted | height | 66.3 | 73.9 | 79.2 | 2.9% | 12 | 368x358 3%, 368x298 15%, 368x224 31%, 245x224 37%, 184x224 15% | 9.27 | 0.5589 |

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
