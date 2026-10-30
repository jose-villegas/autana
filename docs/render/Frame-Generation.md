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
scorer ([Bake-Method.md](Bake-Method.md)): mean CIE76 dE and luma SSIM.
Repeating the last frame, what the panel shows without frame generation, is
scored beside it.

<details><summary>Every scene and size</summary>

<!-- generated: framegen-scores sha256=f3353ad062fc8fab3b63e4efef8463fc8cf8767dd947f9351e0ca5d1997d6512 -->
| Scene | Size | Ahead | Holes % (pinholes) | dE warp, row fill | dE warp, previous fill | dE repeat | p95 frame dE warp / repeat | SSIM warp / repeat |
|---|---|---|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 1 | 8.0 (4.0) | 3.84 | 3.10 | 2.35 | 17.22 / 5.00 | 0.815 / 0.879 |
| bistro-fitted-full | 184x224 | 2 | 13.4 (6.1) | 4.94 | 3.75 | 3.69 | 20.55 / 7.88 | 0.766 / 0.798 |
| bistro-fitted-full | 184x149 | 1 | 7.9 (3.9) | 3.94 | 3.15 | 2.35 | 17.24 / 5.01 | 0.809 / 0.883 |
| bistro-fitted-full | 184x149 | 2 | 13.4 (6.1) | 5.06 | 3.84 | 3.68 | 20.54 / 7.86 | 0.760 / 0.803 |
| bistro-fitted-full | 184x112 | 1 | 8.0 (4.0) | 3.99 | 3.18 | 2.35 | 17.32 / 5.02 | 0.804 / 0.886 |
| bistro-fitted-full | 184x112 | 2 | 13.3 (6.0) | 5.19 | 3.90 | 3.68 | 20.55 / 7.84 | 0.750 / 0.807 |
| sponza-fitted-full | 184x224 | 1 | 6.9 (5.0) | 4.35 | 3.71 | 2.94 | 9.25 / 6.02 | 0.802 / 0.888 |
| sponza-fitted-full | 184x224 | 2 | 12.3 (8.0) | 5.77 | 4.62 | 4.59 | 12.50 / 9.20 | 0.737 / 0.807 |
| sponza-fitted-full | 184x149 | 1 | 6.9 (4.9) | 4.48 | 3.81 | 2.94 | 9.29 / 6.06 | 0.796 / 0.890 |
| sponza-fitted-full | 184x149 | 2 | 12.3 (8.0) | 5.97 | 4.75 | 4.58 | 12.80 / 9.21 | 0.722 / 0.808 |
| sponza-fitted-full | 184x112 | 1 | 6.7 (4.9) | 4.58 | 3.88 | 2.93 | 9.56 / 6.03 | 0.790 / 0.893 |
| sponza-fitted-full | 184x112 | 2 | 12.3 (7.9) | 6.11 | 4.86 | 4.57 | 13.32 / 9.21 | 0.717 / 0.811 |
| sponza | 184x224 | 1 | 5.4 (3.9) | 3.41 | 2.92 | 2.74 | 10.61 / 5.84 | 0.832 / 0.883 |
| sponza | 184x224 | 2 | 9.6 (6.1) | 4.27 | 3.45 | 4.25 | 13.32 / 8.78 | 0.789 / 0.802 |
| sponza | 184x149 | 1 | 5.4 (3.9) | 3.55 | 3.04 | 2.74 | 10.59 / 5.83 | 0.826 / 0.884 |
| sponza | 184x149 | 2 | 9.6 (6.1) | 4.44 | 3.59 | 4.25 | 13.38 / 8.76 | 0.778 / 0.803 |
| sponza | 184x112 | 1 | 5.3 (3.9) | 3.63 | 3.11 | 2.73 | 10.55 / 5.84 | 0.823 / 0.887 |
| sponza | 184x112 | 2 | 9.6 (6.1) | 4.59 | 3.72 | 4.25 | 13.40 / 8.77 | 0.772 / 0.805 |
| sponza-flat | 184x224 | 1 | 2.8 (1.8) | 1.81 | 1.63 | 2.37 | 3.60 / 5.68 | 0.850 / 0.811 |
| sponza-flat | 184x224 | 2 | 5.4 (3.5) | 2.40 | 2.10 | 3.96 | 6.25 / 8.84 | 0.816 / 0.704 |
| sponza-flat | 184x149 | 1 | 2.7 (1.8) | 1.92 | 1.74 | 2.38 | 3.59 / 5.69 | 0.847 / 0.815 |
| sponza-flat | 184x149 | 2 | 5.4 (3.5) | 2.55 | 2.24 | 3.97 | 6.27 / 8.83 | 0.808 / 0.708 |
| sponza-flat | 184x112 | 1 | 2.6 (1.8) | 1.98 | 1.81 | 2.39 | 3.53 / 5.70 | 0.845 / 0.821 |
| sponza-flat | 184x112 | 2 | 5.3 (3.4) | 2.72 | 2.40 | 3.97 | 6.26 / 8.83 | 0.802 / 0.713 |
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
| sponza-flat-fitted | 184x224 | 1 | 25.1 (12.7) | 11.88 | 7.37 | 2.41 | 23.02 / 5.60 | 0.490 / 0.855 |
| sponza-flat-fitted | 184x224 | 2 | 38.8 (14.7) | 13.87 | 7.03 | 4.03 | 25.56 / 9.00 | 0.438 / 0.767 |
| sponza-flat-fitted | 184x149 | 1 | 25.0 (12.6) | 12.03 | 7.44 | 2.42 | 23.06 / 5.60 | 0.466 / 0.859 |
| sponza-flat-fitted | 184x149 | 2 | 38.8 (14.6) | 14.03 | 7.09 | 4.04 | 25.55 / 8.98 | 0.416 / 0.769 |
| sponza-flat-fitted | 184x112 | 1 | 25.0 (12.6) | 12.12 | 7.47 | 2.42 | 23.10 / 5.57 | 0.458 / 0.864 |
| sponza-flat-fitted | 184x112 | 2 | 38.8 (14.5) | 14.18 | 7.14 | 4.04 | 25.55 / 8.95 | 0.404 / 0.774 |
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
step on, both cores taking half the rows, and fills the holes. Compose is
the frame's own upscale into the half picture; the send overlaps it.

<!-- generated: framegen-board sha256=22c6e04ff2ccbebac074a209146e63d5f3166699f58b0e3bdb739a1b1468c42c -->
| Scene | Size | Drawn frame | Warp | Fill | Compose | Generated frame |
|---|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 43.5 ms | 9.9 ms | 3.7 ms | 2.5 ms | 16.1 ms |
| bistro-fitted-full | 184x179 | 40.1 ms | 7.6 ms | 2.9 ms | 2.2 ms | 12.7 ms |
| bistro-fitted-full | 184x149 | 37.6 ms | 6.4 ms | 2.5 ms | 2.0 ms | 10.9 ms |
| bistro-fitted-full | 184x112 | 34.7 ms | 4.8 ms | 1.9 ms | 1.7 ms | 8.3 ms |
| sponza-fitted-full | 184x224 | 50.3 ms | 10.0 ms | 3.9 ms | 2.6 ms | 16.4 ms |
| sponza-fitted-full | 184x179 | 45.9 ms | 7.8 ms | 3.0 ms | 2.3 ms | 13.1 ms |
| sponza-fitted-full | 184x149 | 42.9 ms | 6.5 ms | 2.5 ms | 2.0 ms | 11.0 ms |
| sponza-fitted-full | 184x112 | 38.9 ms | 4.8 ms | 1.9 ms | 1.8 ms | 8.4 ms |
<!-- /generated: framegen-board -->

- A generated frame costs a third to two fifths of a drawn one at 184x224
  and about a quarter at 184x112.
- The fill writes the whole generated picture back over the drawn one, colour
  and depth, so compose reads it as a drawn frame; the holes alone are a few percent of the pixels.

Where the warp's time goes. The same warp is timed with every splat off screen
(the clear, the maths and the source reads), to the drawn view itself (adding
every write, in order), and to the next view (adding their scatter):

<!-- generated: framegen-profile sha256=e35c48d3da642a0a93ab88915900db06492325724eaad3f42b445b5cb9704608 -->
| Scene | Size | Clear | Off screen | To the drawn view | To the next view |
|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 0.8 ms | 10.2 ms | 9.8 ms | 9.9 ms |
| bistro-fitted-full | 184x179 | 0.6 ms | 7.3 ms | 7.7 ms | 7.6 ms |
| bistro-fitted-full | 184x149 | 0.5 ms | 6.1 ms | 6.4 ms | 6.4 ms |
| bistro-fitted-full | 184x112 | 0.3 ms | 4.6 ms | 4.8 ms | 4.8 ms |
| sponza-fitted-full | 184x224 | 0.8 ms | 10.3 ms | 9.8 ms | 10.0 ms |
| sponza-fitted-full | 184x179 | 0.6 ms | 7.3 ms | 7.8 ms | 7.8 ms |
| sponza-fitted-full | 184x149 | 0.5 ms | 6.1 ms | 6.4 ms | 6.5 ms |
| sponza-fitted-full | 184x112 | 0.4 ms | 4.6 ms | 4.8 ms | 4.8 ms |
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

<!-- generated: framegen-modes sha256=0c3104683b002e809c0512bc7ab9ccb051daed685bf9ea02debb3bf346814911 -->
| Scene | Frame generation | Budget | Shown | p95 frame | Drawn | Drawn height |
|---|---|---|---|---|---|---|
| bistro-fitted-full | off | off | 26.4 fps | 58.0 ms | 100% | 224 |
| bistro-fitted-full | on | off | 30.1 fps | 55.9 ms | 50% | 224 |
| bistro-fitted-full | off | 23 ms | 30.3 fps | 47.3 ms | 100% | 152 |
| bistro-fitted-full | on | 23 ms | 33.3 fps | 46.6 ms | 50% | 180 |
| bistro-fitted-full | on | 17 ms | 36.8 fps | 46.2 ms | 50% | 139 |
| bistro-fitted-full | on | 11 ms | 39.3 fps | 45.9 ms | 50% | 115 |
| sponza-fitted-full | off | off | 20.8 fps | 60.0 ms | 100% | 224 |
| sponza-fitted-full | on | off | 26.5 fps | 59.3 ms | 50% | 224 |
| sponza-fitted-full | off | 23 ms | 26.5 fps | 46.5 ms | 100% | 116 |
| sponza-fitted-full | on | 23 ms | 32.0 fps | 46.0 ms | 50% | 146 |
| sponza-fitted-full | on | 17 ms | 34.9 fps | 45.9 ms | 50% | 115 |
| sponza-fitted-full | on | 11 ms | 35.3 fps | 45.8 ms | 50% | 112 |
<!-- /generated: framegen-modes -->

- At the same budget, frame generation draws finer and shows more frames:
  Bistro draws 180 rows instead of 152 and shows 10% more frames.
- A tighter budget turns the saving into rate: Bistro shows nearly 40 fps at
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
