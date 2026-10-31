# Frame generation by depth reprojection

Frame generation shows a warped copy of the last drawn frame between drawn
frames, so more frames reach the panel without drawing more triangles. Each
drawn frame's colour and depth are forward-warped to the camera view of the
next update<sup>[[97]](../Citations.md#mark-1997)</sup>. The warp
extrapolates rather than interpolates, so it adds no frame of
latency<sup>[[98]](../Citations.md#meta-asw-2016)</sup>.

The view is the camera's exact pose at that update, so a scene whose objects
do not move on their own needs no per-pixel motion vectors. `render/framegen/` holds the warp, and the render
context runs it: `render_context_set_frame_generation()` turns it on.

```mermaid
flowchart LR
    D["Drawn frame N<br/>colour + depth, its view"] --> W["Forward warp<br/>to update N + 1's view"]
    V["Camera at update N + 1"] --> W
    W --> F["Holes take frame N's pixel"]
    F --> C["Compose and present<br/>as a drawn frame"]
    C --> D2["Drawn frame N + 2"]
```

## Method

| Step | What it does |
|---|---|
| Warp | Each pixel goes back to the world through its depth and the view it was drawn through, then into the new view, and lands on the nearest pixel; the nearer point wins. The sky has no depth and only turns |
| Holes | Pixels nothing lands on: pinholes where the warp stretches the picture, background uncovered behind a near edge, and the screen edge |
| Fill | A hole keeps the drawn frame's pixel at the same place |
| Compose | The generated picture goes through the same upscale and present as a drawn one |

Lighting is baked, so a surface's colour does not change with the view and
a warped pixel is the right colour. Depth-based warps for head motion work
the same way<sup>[[100]](../Citations.md#meta-asw2-2019)</sup>.

A filled hole is the frame before's pixel, not the background it uncovered.
Filling holes from neighbouring background instead<sup>[[99]](../Citations.md#didyk-2010)</sup>
scored worse on the host, and filling the pinholes from their neighbours first
gained less than 0.02 dE: what error is left comes from the warp's
nearest-pixel resampling, not from holes.

## Picture

On the host, every scene's camera path is drawn at each half-picture size of
the [dynamic-resolution](Dynamic-Resolution.md) ladder, one frame per display
step of a 30 fps display. A sample of frames is warped one and two steps
ahead and scored against the frame really drawn there with the reference
scorer ([Bake-Method.md](offline/Bake-Method.md)): mean CIE76 dE and luma SSIM.
Repeating the last frame, what the panel shows without frame generation, is
scored beside it.

<details><summary>Every scene and size</summary>

<!-- generated: framegen-scores sha256=86a77ccde37330fa0ced6aad9412e3d0a49a5cdb7363f38162fb59bbf98725fe -->
| Scene | Size | Ahead | Holes % (pinholes) | dE warp, row fill | dE warp, previous fill | dE repeat | p95 frame dE warp / repeat | SSIM warp / repeat |
|---|---|---|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 1 | 2.8 (2.2) | 1.67 | 1.63 | 2.36 | 2.83 / 5.06 | 0.918 / 0.878 |
| bistro-fitted-full | 184x224 | 2 | 5.5 (4.0) | 2.12 | 2.07 | 3.69 | 3.19 / 7.88 | 0.892 / 0.798 |
| bistro-fitted-full | 184x149 | 1 | 2.7 (2.1) | 1.76 | 1.71 | 2.36 | 2.93 / 5.11 | 0.917 / 0.882 |
| bistro-fitted-full | 184x149 | 2 | 5.4 (4.0) | 2.27 | 2.21 | 3.69 | 3.52 / 7.86 | 0.887 / 0.803 |
| bistro-fitted-full | 184x112 | 1 | 2.8 (2.2) | 1.82 | 1.77 | 2.36 | 3.12 / 5.10 | 0.915 / 0.885 |
| bistro-fitted-full | 184x112 | 2 | 5.3 (3.9) | 2.40 | 2.33 | 3.68 | 3.94 / 7.84 | 0.879 / 0.807 |
| sponza-fitted-full | 184x224 | 1 | 2.2 (1.3) | 2.13 | 2.01 | 2.95 | 4.01 / 6.02 | 0.921 / 0.887 |
| sponza-fitted-full | 184x224 | 2 | 4.4 (2.7) | 2.52 | 2.35 | 4.59 | 4.29 / 9.20 | 0.902 / 0.807 |
| sponza-fitted-full | 184x149 | 1 | 2.1 (1.3) | 2.28 | 2.15 | 2.94 | 4.18 / 6.06 | 0.917 / 0.890 |
| sponza-fitted-full | 184x149 | 2 | 4.4 (2.7) | 2.73 | 2.54 | 4.58 | 4.48 / 9.21 | 0.897 / 0.808 |
| sponza-fitted-full | 184x112 | 1 | 2.0 (1.3) | 2.38 | 2.25 | 2.94 | 4.44 / 6.03 | 0.916 / 0.892 |
| sponza-fitted-full | 184x112 | 2 | 4.3 (2.6) | 2.90 | 2.71 | 4.57 | 4.73 / 9.21 | 0.893 / 0.811 |
| sponza | 184x224 | 1 | 2.2 (1.4) | 1.99 | 1.88 | 2.74 | 3.59 / 5.84 | 0.915 / 0.883 |
| sponza | 184x224 | 2 | 4.4 (2.8) | 2.35 | 2.19 | 4.25 | 3.91 / 8.78 | 0.897 / 0.802 |
| sponza | 184x149 | 1 | 2.2 (1.3) | 2.12 | 2.01 | 2.74 | 3.87 / 5.83 | 0.911 / 0.884 |
| sponza | 184x149 | 2 | 4.4 (2.8) | 2.55 | 2.37 | 4.25 | 4.04 / 8.76 | 0.889 / 0.803 |
| sponza | 184x112 | 1 | 2.1 (1.3) | 2.22 | 2.10 | 2.74 | 4.05 / 5.84 | 0.909 / 0.887 |
| sponza | 184x112 | 2 | 4.3 (2.7) | 2.70 | 2.53 | 4.25 | 4.25 / 8.77 | 0.885 / 0.805 |
| sponza-flat | 184x224 | 1 | 2.2 (1.4) | 1.44 | 1.33 | 2.37 | 2.14 / 5.68 | 0.877 / 0.811 |
| sponza-flat | 184x224 | 2 | 4.4 (2.8) | 1.77 | 1.62 | 3.96 | 2.65 / 8.84 | 0.855 / 0.704 |
| sponza-flat | 184x149 | 1 | 2.2 (1.3) | 1.56 | 1.44 | 2.38 | 2.33 / 5.69 | 0.872 / 0.815 |
| sponza-flat | 184x149 | 2 | 4.4 (2.8) | 1.95 | 1.79 | 3.97 | 2.85 / 8.83 | 0.845 / 0.708 |
| sponza-flat | 184x112 | 1 | 2.1 (1.3) | 1.62 | 1.53 | 2.39 | 2.57 / 5.70 | 0.869 / 0.821 |
| sponza-flat | 184x112 | 2 | 4.3 (2.7) | 2.12 | 1.95 | 3.98 | 3.08 / 8.83 | 0.839 / 0.713 |
| sponza-lite | 184x224 | 1 | 2.2 (1.4) | 1.69 | 1.58 | 2.30 | 3.21 / 4.69 | 0.925 / 0.894 |
| sponza-lite | 184x224 | 2 | 4.5 (2.8) | 2.05 | 1.88 | 3.61 | 3.71 / 7.15 | 0.905 / 0.824 |
| sponza-lite | 184x149 | 1 | 2.2 (1.4) | 1.80 | 1.69 | 2.30 | 3.44 / 4.68 | 0.922 / 0.896 |
| sponza-lite | 184x149 | 2 | 4.5 (2.8) | 2.20 | 2.02 | 3.61 | 3.81 / 7.16 | 0.899 / 0.824 |
| sponza-lite | 184x112 | 1 | 2.1 (1.4) | 1.87 | 1.77 | 2.31 | 3.67 / 4.67 | 0.919 / 0.898 |
| sponza-lite | 184x112 | 2 | 4.4 (2.7) | 2.33 | 2.16 | 3.61 | 4.01 / 7.15 | 0.896 / 0.825 |
| sponza-fitted | 184x224 | 1 | 2.2 (1.4) | 1.90 | 1.77 | 2.63 | 3.38 / 5.34 | 0.925 / 0.893 |
| sponza-fitted | 184x224 | 2 | 4.4 (2.7) | 2.25 | 2.08 | 4.13 | 3.72 / 8.26 | 0.908 / 0.820 |
| sponza-fitted | 184x149 | 1 | 2.2 (1.3) | 2.01 | 1.90 | 2.64 | 3.55 / 5.34 | 0.922 / 0.895 |
| sponza-fitted | 184x149 | 2 | 4.4 (2.7) | 2.44 | 2.25 | 4.13 | 3.87 / 8.25 | 0.902 / 0.821 |
| sponza-fitted | 184x112 | 1 | 2.1 (1.3) | 2.12 | 1.99 | 2.63 | 3.77 / 5.33 | 0.920 / 0.898 |
| sponza-fitted | 184x112 | 2 | 4.3 (2.7) | 2.61 | 2.41 | 4.13 | 4.07 / 8.22 | 0.898 / 0.824 |
| sponza-flat-fitted | 184x224 | 1 | 2.2 (1.3) | 1.61 | 1.49 | 2.43 | 3.20 / 5.60 | 0.897 / 0.854 |
| sponza-flat-fitted | 184x224 | 2 | 4.4 (2.7) | 2.00 | 1.82 | 4.04 | 3.60 / 8.99 | 0.876 / 0.766 |
| sponza-flat-fitted | 184x149 | 1 | 2.1 (1.3) | 1.75 | 1.62 | 2.44 | 3.35 / 5.61 | 0.893 / 0.858 |
| sponza-flat-fitted | 184x149 | 2 | 4.4 (2.7) | 2.19 | 2.00 | 4.05 | 3.86 / 8.98 | 0.869 / 0.768 |
| sponza-flat-fitted | 184x112 | 1 | 2.0 (1.3) | 1.84 | 1.72 | 2.44 | 3.62 / 5.57 | 0.890 / 0.862 |
| sponza-flat-fitted | 184x112 | 2 | 4.3 (2.6) | 2.35 | 2.16 | 4.06 | 4.01 / 8.95 | 0.865 / 0.773 |
<!-- /generated: framegen-scores -->

</details>

- The warp beats repeating the frame on every scene and size: the mean dE
  drops by a quarter to a half, and the worst frames' dE (p95) by a quarter
  to over two thirds, most at two steps ahead.
- One step ahead leaves 2-3% holes, most of them pinholes; two steps ahead
  doubles both.
- Smaller render heights keep the gain; the warp's error grows a little with
  the coarser grid.

Bistro along its whole path at 184x224: where repeating the frame jumps
(turns and fast passes), the generated frame stays close to the true one.

![Bistro: mean dE of the generated frame against the true one along the path](../images/render/framegen-bistro-path.png)

The frame with the most holes that are not pinholes, and the frame the
generated one strays from most, against the true frame, with where they
differ most enlarged:

![Bistro: true and generated frames with their difference](../images/render/framegen-bistro-sheet.png)

![Bistro: the places the generated frame differs most, enlarged](../images/render/framegen-bistro-sheet.crops.png)

The holes themselves, in red over the warp, before the fill:

![Bistro: true frame, generated frame and its holes](../images/render/framegen-bistro-holes.png)

![Sponza: true frame, generated frame and its holes](../images/render/framegen-sponza-holes.png)

The screen edge opens most holes on a turn: a guard band, drawing a few
pixels past each edge, would remove them. Uncovered background behind thin
near objects (a lamp post, chair legs) is what is left.

![Bistro: the generated frames around the one with the most holes](../images/render/framegen-bistro.gif)

## Cost on the board

The on-request `run_framegen_perf_suite` draws each path at every
half-picture step. At 24 poses along it, it warps the drawn frame one display
step on, both cores taking half the rows, fills the holes and composes the
generated picture into the half picture; the send overlaps it.

<!-- generated: framegen-board sha256=01f6f82b80a4a269458174d0d54e5a9ae6cf3e93c367ab404630ea5550330c7d -->
| Scene | Size | Drawn frame | Warp | Fill | Compose | Generated frame |
|---|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 43.7 ms | 9.8 ms | 2.1 ms | 2.8 ms | 14.7 ms |
| bistro-fitted-full | 184x179 | 40.1 ms | 7.6 ms | 1.6 ms | 2.4 ms | 11.6 ms |
| bistro-fitted-full | 184x149 | 37.8 ms | 6.4 ms | 1.4 ms | 2.1 ms | 9.9 ms |
| bistro-fitted-full | 184x112 | 34.8 ms | 4.7 ms | 1.0 ms | 1.9 ms | 7.6 ms |
| sponza-fitted-full | 184x224 | 50.5 ms | 9.9 ms | 1.8 ms | 2.8 ms | 14.6 ms |
| sponza-fitted-full | 184x179 | 46.1 ms | 7.7 ms | 1.5 ms | 2.4 ms | 11.6 ms |
| sponza-fitted-full | 184x149 | 43.0 ms | 6.4 ms | 1.2 ms | 2.1 ms | 9.7 ms |
| sponza-fitted-full | 184x112 | 39.1 ms | 4.8 ms | 0.9 ms | 1.8 ms | 7.5 ms |
<!-- /generated: framegen-board -->

- A generated frame costs about a third of a drawn one at 184x224 and a
  fifth at 184x112.
- The fill writes only the holes, a few percent of the pixels, from the
  drawn frame into the warp's own picture, and compose reads that picture
  (`raster_upscale_picture()`). The drawn frame stays as it was drawn.

Where the warp's time goes. The same warp is timed with every splat off screen
(the clear, the maths and the source reads), to the drawn view itself (adding
every write, in order), and to the next view (adding their scatter):

<!-- generated: framegen-profile sha256=300ef29aefd22c36e45d227172c621c578d7bc14289981c18be07f192b1a4b90 -->
| Scene | Size | Clear | Off screen | To the drawn view | To the next view |
|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 0.8 ms | 10.2 ms | 9.8 ms | 9.8 ms |
| bistro-fitted-full | 184x179 | 0.6 ms | 7.2 ms | 7.7 ms | 7.6 ms |
| bistro-fitted-full | 184x149 | 0.5 ms | 6.0 ms | 6.4 ms | 6.4 ms |
| bistro-fitted-full | 184x112 | 0.3 ms | 4.6 ms | 4.8 ms | 4.7 ms |
| sponza-fitted-full | 184x224 | 0.8 ms | 10.2 ms | 9.7 ms | 9.9 ms |
| sponza-fitted-full | 184x179 | 0.6 ms | 7.3 ms | 7.7 ms | 7.7 ms |
| sponza-fitted-full | 184x149 | 0.5 ms | 6.0 ms | 6.4 ms | 6.4 ms |
| sponza-fitted-full | 184x112 | 0.3 ms | 4.6 ms | 4.8 ms | 4.8 ms |
<!-- /generated: framegen-profile -->

The three warps cost the same: writing and scattering the splats is free next
to the maths. The warp is bound by its arithmetic, a reciprocal and about ten
multiply-adds per pixel, so it takes two pixels a step and lets their
dependent chains overlap. The reciprocal keeps 14 bits
(`mathf_recip_approx`), a hundredth of a pixel across the picture, which
saves 6% of the warp over a full-precision one.

## When it pays

A generated frame only helps when it costs less than a drawn one. A scene
that draws faster than the warp runs would show fewer frames with generation
on, so the render context times both and decides each frame:

| Last measured | Next frame after a drawn one |
|---|---|
| No generated frame yet | Generated |
| The generated frame cost under (100 − `FRAMEGEN_GAIN_PERCENT`)% of the drawn one | Generated |
| Already generating, and it still costs less than the drawn one | Generated |
| It saved less | Drawn, priced alone at one frame's budget |
| `FRAMEGEN_RETRY_DRAWN` drawn frames in a row | Generated, to measure it again |

The margin keeps a generated frame, which shows the scene less exactly than
a drawn one, from being shown for a saving too small to see.

## With dynamic resolution

A [dynamic-resolution](Dynamic-Resolution.md) budget is the cost of a shown
frame. With frame generation on, a drawn frame and the one generated from it
are shown in the time of two, so the render context prices each drawn frame
against what the pair may spend. With B the budget per shown frame, g the
last generated frame's cost and N = `FRAMEGEN_SHOWN_PER_DRAWN` frames shown
per drawn one, while generation pays:

| Quantity | Value |
|---|---|
| A drawn frame's budget | max(N B − (N − 1) g, B) |
| The pair's cost when the policy holds it | N B: B per shown frame |
| A generated frame's own cost | not priced: it only sets g |

```mermaid
flowchart LR
    G["Generated frame<br/>costs g"] -->|"g"| P["Policy prices the next<br/>drawn frame against<br/>N B - (N - 1) g, never below B"]
    P --> S["Drawn frame at the<br/>step that fits"]
    S --> G2["Generated frame"]
```

The floor of one frame's budget keeps a slow generated frame from starving
the drawn one. The drawn frame's budget against the generated frame's cost,
with this board's generated frames marked:

![A drawn frame's budget against the generated frame's cost](../images/render/framegen-pricing.png)

The budget sets the trade. At the same budget the saving buys resolution: the
drawn frame gets more than one frame's time and draws a finer step. At a
tighter budget it buys rate: the pair still fits, at more frames per second.
Each scene's path played in real time on the board, per mode:

<!-- generated: framegen-modes sha256=c171204d7caa76807d8c6b3aca076e88ea65c958261997295e2f4fa8996e9cf1 -->
| Scene | Frame generation | Budget | Shown | p95 frame | Drawn | Drawn height |
|---|---|---|---|---|---|---|
| bistro-fitted-full | off | off | 26.4 fps | 57.9 ms | 100% | 224 |
| bistro-fitted-full | on | off | 31.4 fps | 55.8 ms | 56% | 224 |
| bistro-fitted-full | off | 23 ms | 30.3 fps | 47.3 ms | 100% | 152 |
| bistro-fitted-full | on | 23 ms | 34.0 fps | 46.6 ms | 56% | 190 |
| bistro-fitted-full | on | 17 ms | 37.3 fps | 46.3 ms | 55% | 154 |
| bistro-fitted-full | on | 11 ms | 40.3 fps | 46.0 ms | 54% | 119 |
| sponza-fitted-full | off | off | 20.8 fps | 60.0 ms | 100% | 224 |
| sponza-fitted-full | on | off | 27.4 fps | 59.4 ms | 50% | 224 |
| sponza-fitted-full | off | 23 ms | 26.5 fps | 46.5 ms | 100% | 116 |
| sponza-fitted-full | on | 23 ms | 31.9 fps | 46.1 ms | 50% | 157 |
| sponza-fitted-full | on | 17 ms | 35.6 fps | 45.9 ms | 50% | 117 |
| sponza-fitted-full | on | 11 ms | 36.2 fps | 45.9 ms | 50% | 112 |
<!-- /generated: framegen-modes -->

- At the same budget, frame generation draws finer and shows more frames:
  Bistro draws 190 rows instead of 152 and shows 12% more frames.
- A tighter budget turns the saving into rate: Bistro shows 40 fps at
  the floor of the ladder.
- The rate gains less than the cost table suggests: every update, drawn or
  generated, also pays the frame's own work outside the render, a few
  milliseconds.

## Pacing

A generated frame is shown as soon as it is made, right after the drawn frame
before it. The shown frames alternate between a drawn frame's interval and a
generated frame's. Each frame's view is right for when it is shown, so motion
stays correct, but the intervals are uneven.

Even intervals need the generated frame presented halfway through the next
drawn frame, while both cores draw it: a slot in the frame loop, not in the
render context, and the frame loop has no such slot.

## For an app

Frame generation and the budget are per render context, set by whoever owns
it: an app for its scenes, a player setting, or later a default the frame
loop applies.

<!-- generated: render-framegen-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Frame generation with dynamic resolution: while it pays, every second frame
 * is warped from the one drawn before it, and each drawn frame is priced for
 * the pair. */
#include "render/context/render_context.h"

static void
framegen_example(render_context_t* context, const resolution_config_t* ladder) {
    render_context_set_dynamic_resolution(context, ladder, NULL, 0); /* ladder->budget_us: per shown frame */
    render_context_set_frame_generation(context, true);
}

/* After a frame: whether it was generated rather than drawn. */
static bool
framegen_example_generated(const render_context_t* context) {
    return render_context_frame(context).generated;
}
```
<!-- /generated: render-framegen-example -->

| Call | Use |
|---|---|
| `render_context_set_frame_generation(context, on)` | On for scenes whose objects do not move on their own; off costs nothing. The context generates a frame only [when it pays](#when-it-pays); a debug view, a camera cut or a new viewport draws the next frame instead |
| `render_context_set_dynamic_resolution(context, config, model, step)` | `config->budget_us` stays the budget per shown frame; with generation on, the context prices each drawn frame for the pair |
| `render_context_frame(context).generated` | Whether the last frame was generated rather than drawn |

## Moving objects

The warp moves everything with the camera, so an object moving on its own
lands where it was. What it would need, not built:

- A rigid object: one map per moved instance, which the
  [motion-vector attachment](Mesh-Rendering.md#motion-vectors) already builds
  from the previous transform, taking only its own pixels.
- A skinned one: a map per bone. Each triangle writes its dominant bone to a
  tag plane, and each pixel moves by that bone's change over the step.
- Tags written only for the moving instances keep the cost to the object's
  pixels, not the whole frame's.

## Reproducing

[`launcher/tools/r3d/framegen/`](../../launcher/tools/r3d/framegen/) holds
the host experiment, with the host scene renderer's `--dump` option. The
board numbers come from the on-request `run_framegen_perf_suite` and
`run_framegen_modes_suite`, captured in
[`docs/render/data/framegen-board.log`](data/framegen-board.log).

| File | Role |
|---|---|
| [`warp.py`](../../launcher/tools/r3d/framegen/warp.py) | The warp and the fills, on a raster dump |
| [`framegen_score.py`](../../launcher/tools/r3d/framegen/framegen_score.py) | One scene and size: render, warp, score; stills, sheet, clip and video |
| [`framegen_report.py`](../../launcher/tools/r3d/framegen/framegen_report.py) | The tables and figures, from the scores and the board capture |
| [`framegen_docs.sh`](../../launcher/tools/r3d/framegen/framegen_docs.sh) | Regenerates this page's tables and images |
