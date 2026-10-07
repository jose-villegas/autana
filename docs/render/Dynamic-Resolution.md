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
render context culls once, before it chooses a size, and draws from that
list, so pricing a frame from what culling kept costs the predictor nothing
extra.

## Where a frame's time goes at each size

The raster brackets its stages for [frame cost](../tools/Frame-Cost.md):
`r3d.cull`, `r3d.transform`, `r3d.draw` and `r3d.upscale`; through the
render context the cull is the census, `r3d.census`. The suite behind
these tables draws the test scene's path at every size, both cores, and then
once more on one core with the span rasterizer stopped after each stage.

<!-- generated: dynres-stages sha256=fe4c2bb9e556b8ba11be8a07966e5285b97ae603700cdca7270409bd3cc270ae -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 108.6 | 109.5 | 128.6 | 0.6 | 4.4 | 93.7 | 9.9 | 33.5 | 33.4 | 33.4 | 48.8 |
| 294x358 | 1.25 | 64% | 86.3 | 86.5 | 104.7 | 0.7 | 4.4 | 70.9 | 10.3 | 31.9 | 27.2 | 25.6 | 28.3 |
| 276x336 | 1.33 | 56% | 81.4 | 81.5 | 99.2 | 0.6 | 4.4 | 66.0 | 10.3 | 31.5 | 25.6 | 23.6 | 24.8 |
| 245x298 | 1.50 | 44% | 72.6 | 72.4 | 88.9 | 0.6 | 4.4 | 58.0 | 9.6 | 30.6 | 23.0 | 20.4 | 18.0 |
| 210x256 | 1.75 | 33% | 65.3 | 66.1 | 79.9 | 0.6 | 4.4 | 50.9 | 9.3 | 29.6 | 19.9 | 16.8 | 13.7 |
| 184x224 | 2.00 | 25% | 54.1 | 54.8 | 66.7 | 0.7 | 4.4 | 43.1 | 5.9 | 28.6 | 17.6 | 14.2 | 8.3 |
| 147x179 | 2.50 | 16% | 48.8 | 49.5 | 60.3 | 0.7 | 4.4 | 35.2 | 8.5 | 27.0 | 14.3 | 10.6 | 3.8 |
| 122x149 | 3.02 | 11% | 43.7 | 44.4 | 53.7 | 0.7 | 4.4 | 30.2 | 8.4 | 25.6 | 12.0 | 8.2 | 1.7 |
| 368x224 | 1.00 x 2.00 | 50% | 67.5 | 67.2 | 82.9 | 0.7 | 4.4 | 55.2 | 7.2 | 31.2 | 18.8 | 16.6 | 21.0 |
| 184x448 | 2.00 x 1.00 | 50% | 83.2 | 83.7 | 98.4 | 0.6 | 4.4 | 70.8 | 7.3 | 31.3 | 31.9 | 28.8 | 21.9 |
| 245x224 | 1.50 x 2.00 | 33% | 62.0 | 62.3 | 76.4 | 0.6 | 4.4 | 47.5 | 9.4 | 29.6 | 18.1 | 15.3 | 12.7 |
| 184x298 | 2.00 x 1.50 | 33% | 63.7 | 64.3 | 78.1 | 0.6 | 4.4 | 52.2 | 6.4 | 29.7 | 22.4 | 19.0 | 12.2 |

Milliseconds; both cores unless marked one core.
<!-- /generated: dynres-stages -->

The split compares neighbouring ladder steps and the floor and recovery
against their isotropic cost references. Both the height-first and width-first
ladders use only full or half panel width, so every step upscales whole rows
or pixel pairs down a row map. The mapped sizes remain in the stage table as
cost references. Rows and span setup follow the height; the height-first
ladder cuts it before cutting the width. The one-core setup stage is the
floor no step goes under.

The ladders are defined in `sponza_content.c` and
`suite_raster_scale_perf.c`. Both use 184x179 as the floor and reserve 184x149
for recovery. Their cost targets are the isotropic 147x179 and 122x149
counterparts, called 2.5x and 3x; those names describe cost, not pixel scale.

<!-- generated: dynres-findings sha256=4d33a41910767f4d80a7b3a3426d37abd73ec023fab27bcb0dee7a63afc266b1 -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| 1.5x over 2x | 245x298 minus 184x224 | 18.5 | 14.8 | 3.7 | 5.3 | 6.2 | 9.6 |
| 2x over 2.5x | 184x224 minus 147x179 | 5.3 | 7.9 | -2.6 | 3.3 | 3.6 | 4.5 |
| full height over half | 368x448 minus 368x224 | 41.2 | 38.5 | 2.7 | 14.5 | 16.9 | 27.8 |
| full width over half | 368x448 minus 184x448 | 25.4 | 22.9 | 2.6 | 1.5 | 4.7 | 27.0 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew the whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=0aa73aa209df50104b8b5ddf83836d1e4a005981e8eab19bb89a3200ea9a7618 -->
| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|
| 60.0 | fixed | half | 54.8 | 66.5 | 68.6 | 25.3% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 60.0 | stepped | isotropic | 52.1 | 60.7 | 66.9 | 10.2% | 11 | 245x298 3%, 210x256 17%, 184x224 27%, 147x179 53% | 9.38 | 0.5502 |
| 60.0 | predicted | isotropic | 54.6 | 60.4 | 64.7 | 7.2% | 14 | 294x358 1%, 245x298 9%, 210x256 18%, 184x224 32%, 147x179 39% | 9.37 | 0.5514 |
| 60.0 | predicted | height | 53.9 | 60.4 | 63.2 | 7.0% | 14 | 368x298 2%, 368x224 15%, 245x224 16%, 184x224 28%, 147x179 39% | 9.35 | 0.5526 |
| 75.0 | fixed | half | 54.8 | 66.5 | 68.6 | 0.0% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 75.0 | stepped | isotropic | 65.7 | 74.8 | 82.5 | 4.7% | 12 | 368x448 2%, 294x358 11%, 245x298 19%, 210x256 41%, 184x224 28% | 9.30 | 0.5564 |
| 75.0 | predicted | isotropic | 66.5 | 73.3 | 77.9 | 1.5% | 10 | 294x358 15%, 245x298 27%, 210x256 39%, 184x224 20% | 9.30 | 0.5566 |
| 75.0 | predicted | height | 66.1 | 73.7 | 81.3 | 2.9% | 12 | 368x358 4%, 368x298 19%, 368x224 38%, 245x224 25%, 184x224 15% | 9.27 | 0.5594 |

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
  counts. A full-width, half-height render scores closer to the reference
  than its isotropic cost reference, so the height-first ladder buys
  more picture for the same milliseconds.
- **Recovery.** The recovery step is available only past the panic share.
  Its cost target is the 3x isotropic reference; the stage split checks it
  against the setup floor.

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
