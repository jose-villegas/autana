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

<!-- generated: dynres-stages sha256=2e193d0b0b4d0e17b5162ba6f25510aca0fa64c3ba135f61de9a6a85004b782b -->
| Render size | Divisor | Pixels | Frame mean | p50 | max | cull | transform | draw | upscale | 1 core: setup | rows | span setup | fill |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 368x448 | 1.00 | 100% | 107.0 | 108.2 | 126.7 | 0.6 | 4.3 | 91.7 | 10.3 | 32.8 | 33.5 | 33.5 | 44.7 |
| 294x358 | 1.25 | 64% | 84.2 | 84.7 | 102.3 | 0.6 | 4.4 | 69.4 | 9.7 | 31.3 | 27.3 | 25.5 | 25.6 |
| 276x336 | 1.33 | 56% | 79.4 | 79.8 | 97.2 | 0.6 | 4.4 | 64.7 | 9.7 | 30.9 | 25.8 | 23.7 | 22.4 |
| 245x298 | 1.50 | 44% | 70.8 | 70.8 | 87.1 | 0.6 | 4.4 | 56.8 | 8.9 | 30.1 | 23.1 | 20.4 | 16.1 |
| 210x256 | 1.75 | 33% | 63.6 | 64.7 | 78.1 | 0.7 | 4.4 | 49.9 | 8.6 | 29.0 | 20.0 | 16.8 | 12.3 |
| 184x224 | 2.00 | 25% | 53.3 | 53.9 | 65.9 | 0.6 | 4.4 | 42.4 | 5.9 | 28.1 | 17.7 | 14.2 | 7.3 |
| 147x179 | 2.50 | 16% | 47.5 | 48.0 | 58.8 | 0.6 | 4.4 | 34.6 | 7.7 | 26.5 | 14.4 | 10.6 | 3.2 |
| 122x149 | 3.02 | 11% | 42.4 | 43.1 | 52.5 | 0.6 | 4.4 | 29.8 | 7.5 | 25.2 | 12.1 | 8.3 | 1.2 |
| 368x224 | 1.00 x 2.00 | 50% | 66.3 | 66.3 | 81.6 | 0.6 | 4.4 | 54.0 | 7.3 | 30.7 | 18.9 | 16.6 | 19.0 |
| 184x448 | 2.00 x 1.00 | 50% | 81.7 | 82.5 | 96.8 | 0.6 | 4.4 | 69.4 | 7.2 | 30.7 | 32.0 | 28.8 | 19.5 |
| 245x224 | 1.50 x 2.00 | 33% | 60.4 | 60.7 | 74.6 | 0.6 | 4.4 | 46.6 | 8.7 | 29.1 | 18.2 | 15.3 | 11.3 |
| 184x298 | 2.00 x 1.50 | 33% | 62.6 | 63.4 | 77.0 | 0.6 | 4.4 | 51.3 | 6.3 | 29.2 | 22.5 | 19.0 | 10.7 |
| 368x358 | 1.00 x 1.25 | 80% | 90.7 | 91.7 | 110.1 | 0.6 | 4.3 | 76.4 | 9.3 | 32.1 | 27.7 | 26.7 | 34.0 |
| 368x298 | 1.00 x 1.50 | 67% | 79.7 | 79.7 | 97.6 | 0.6 | 4.4 | 66.4 | 8.2 | 31.6 | 23.8 | 22.1 | 27.2 |
| 184x358 | 2.00 x 1.25 | 40% | 69.9 | 71.5 | 85.5 | 0.6 | 4.4 | 58.2 | 6.6 | 29.8 | 26.3 | 22.9 | 13.8 |
| 184x179 | 2.00 x 2.50 | 20% | 47.7 | 48.1 | 59.8 | 0.7 | 4.4 | 37.0 | 5.6 | 27.3 | 14.8 | 11.3 | 4.9 |
| 184x149 | 2.00 x 3.01 | 17% | 43.9 | 44.6 | 55.1 | 0.6 | 4.4 | 33.4 | 5.4 | 26.7 | 12.8 | 9.3 | 3.5 |

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

<!-- generated: dynres-findings sha256=4fd510d0ab46353b4630922b4e8da687b92ade8ce79a1fe9d4ff6720ebed985b -->
| Question | Compared | Frame | draw | upscale | 1 core: rows | span setup | fill |
|---|---|---|---|---|---|---|---|
| half width, more height over half | 184x298 minus 184x224 | 9.4 | 8.9 | 0.5 | 4.8 | 4.8 | 3.4 |
| half over floor | 184x224 minus 184x179 | 5.6 | 5.4 | 0.3 | 2.9 | 2.9 | 2.4 |
| floor over 2.5x cost reference | 184x179 minus 147x179 | 0.2 | 2.4 | -2.2 | 0.4 | 0.7 | 1.8 |
| recovery over 3x cost reference | 184x149 minus 122x149 | 1.5 | 3.6 | -2.1 | 0.7 | 1.1 | 2.3 |
| full height over half | 368x448 minus 368x224 | 40.7 | 37.7 | 3.1 | 14.6 | 16.9 | 25.8 |
| full width over half | 368x448 minus 184x448 | 25.3 | 22.2 | 3.1 | 1.5 | 4.7 | 25.2 |
<!-- /generated: dynres-findings -->

## The policies on the board

Each policy flew the whole path at a fixed frame step through the scene
manager, at two budgets; the fixed row is the camera's half scale. The quality
columns score every frame's size against the reference render of the same
pose at full size ([Render-Harness.md](../tools/Render-Harness.md)).

<!-- generated: dynres-policies sha256=6281f3b5feb235867fa7d3d4b1a623e311340d394dc9e30a97bc3961e0a7feac -->
| Budget | Policy | Ladder | p50 | p95 | max | Over budget | Switches | Time at each size | Mean dE | SSIM |
|---|---|---|---|---|---|---|---|---|---|---|
| 60.0 | fixed | half | 54.0 | 65.6 | 67.8 | 23.4% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 60.0 | stepped | width | 51.1 | 60.8 | 67.0 | 10.9% | 13 | 184x448 2%, 184x358 3%, 184x298 18%, 184x224 28%, 184x179 49% | 9.31 | 0.5549 |
| 60.0 | predicted | width | 53.9 | 60.1 | 63.2 | 5.5% | 14 | 184x448 2%, 184x358 13%, 184x298 16%, 184x224 31%, 184x179 37% | 9.31 | 0.5551 |
| 60.0 | predicted | height | 52.5 | 60.1 | 61.9 | 5.8% | 10 | 368x298 3%, 368x224 16%, 184x224 44%, 184x179 37% | 9.30 | 0.5560 |
| 75.0 | fixed | half | 54.0 | 65.6 | 67.8 | 0.0% | 0 | 184x224 100% | 9.30 | 0.5562 |
| 75.0 | stepped | width | 64.4 | 74.5 | 81.9 | 4.1% | 12 | 368x448 2%, 184x448 14%, 184x358 25%, 184x298 34%, 184x224 25% | 9.28 | 0.5574 |
| 75.0 | predicted | width | 66.2 | 73.8 | 77.3 | 1.2% | 10 | 184x448 22%, 184x358 32%, 184x298 29%, 184x224 17% | 9.28 | 0.5576 |
| 75.0 | predicted | height | 65.2 | 74.1 | 80.7 | 2.3% | 12 | 368x448 2%, 368x358 2%, 368x298 25%, 368x224 41%, 184x224 30% | 9.26 | 0.5599 |

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
