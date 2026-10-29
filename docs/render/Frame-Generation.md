# Frame generation by depth reprojection

An experiment: show a warped copy of the last rendered frame between
rendered frames, to raise the displayed rate without drawing more triangles.
This page measures its picture on the host and its cost on the board.
`render_context_set_frame_generation()` puts it in the frame loop: each drawn
frame is followed by one warped from it to that update's own camera view.

The rendered frame's colour and depth are forward-warped to a later camera
pose<sup>[[97]](../Citations.md#mark-1997)</sup>, extrapolated rather than
interpolated so no frame of latency is added<sup>[[98]](../Citations.md#meta-asw-2016)</sup>.
The pose comes from the camera path, so no per-pixel motion vectors are needed.

```mermaid
flowchart LR
    R["Rendered frame N<br/>colour + depth"] --> W["Forward warp<br/>to pose N + 1"]
    P["Camera path<br/>pose N + 1"] --> W
    W --> F["Hole fill"]
    F --> S["Shown between<br/>N and the next render"]
```

## Method

| Step | What it does |
|---|---|
| Warp | Each pixel goes back to the world through its depth and the pose it was drawn at, then into the later pose; the nearest point wins a pixel. The sky has no depth and turns with the camera |
| Holes | Pixels nothing lands on: pinholes where the warp stretches the picture, uncovered background behind a near edge, and the screen edge |
| Row fill | A hole takes the farther of its nearest neighbours left and right, since an uncovered gap shows background<sup>[[99]](../Citations.md#didyk-2010)</sup> |
| Previous fill | A hole keeps the rendered frame's colour at the same pixel |
| Repeat | What the panel shows today between renders, scored as the baseline |

Lighting is baked, so a surface's colour does not change with the view and
a warped pixel is the right colour. Depth-based warps for head motion work
the same way<sup>[[100]](../Citations.md#meta-asw2-2019)</sup>.

## Picture

Every scene's camera path is drawn on the host at each half-picture size of
the [dynamic-resolution](Dynamic-Resolution.md) ladder, one frame per display
step of a 30 fps display. A sample of frames along the path is
warped one and two steps ahead and scored
against the frame really drawn there with the reference scorer
([Bake-Method.md](Bake-Method.md)): mean CIE76 dE and luma SSIM.
The worst frames matter more than the mean, so the 95th percentile of the per-frame mean dE is listed too.

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

What the table shows:

- The warp beats repeating the frame on every scene and size: the mean dE
  drops by a quarter to a half, and the worst frames' dE by a third to two thirds.
- One step ahead leaves 2-3% holes, most of them pinholes the row fill
  closes; two steps ahead doubles both.
- The previous-frame fill scores slightly better than the row fill, at no cost.
- Smaller render heights keep the gain; the warp's error grows a little with the coarser grid.

Bistro along its whole path at 184x224: where repeating the frame jumps
(turns and fast passes, up to dE 9 and 12), the warp stays under 5.

![Bistro: mean dE of the generated frame against the true one along the path](../images/render/framegen-bistro-path.png)

The frame on each path where the warp opens the most holes that are not
pinholes: the true frame, the warp of the frame before with its holes
filled, and the holes in red.

![Bistro: true frame, warped frame and its holes](../images/render/framegen-bistro-holes.png)

![Sponza: true frame, warped frame and its holes](../images/render/framegen-sponza-holes.png)

The screen edge opens most holes on a turn: a guard band, drawing a few
pixels past each edge, would remove them. Uncovered background behind thin
near objects (a lamp post, chair legs) is what is left.

![Bistro: the warp around the frame with the most holes](../images/render/framegen-bistro.gif)

## Cost on the board

The on-request `run_framegen_perf_suite` draws each path at every
half-picture step and, at 24 poses along it, warps the drawn frame one display
step on into a second cleared colour and depth target. The warp runs on one
core and split over both; the hole fill takes the drawn frame's pixel. The
present is the frame's own half-picture expand, and the send overlaps it.

<!-- generated: framegen-board sha256=32bbb147b6f3555d6bd473918f7863cc534a364bf020fc3d0288045f90ae0cde -->
| Scene | Size | Rendered frame | Warp, both cores (one) | Fill | Present | Generated frame | Displayed: render only | Displayed: + 1 generated |
|---|---|---|---|---|---|---|---|---|
| bistro-fitted-full | 184x224 | 43.7 ms | 12.1 ms (21.3) | 2.1 ms | 2.6 ms | 16.8 ms | 23 fps | 33 fps |
| bistro-fitted-full | 184x179 | 40.1 ms | 9.6 ms (16.9) | 1.7 ms | 2.2 ms | 13.5 ms | 25 fps | 37 fps |
| bistro-fitted-full | 184x149 | 37.7 ms | 8.0 ms (14.1) | 1.4 ms | 2.0 ms | 11.4 ms | 27 fps | 41 fps |
| bistro-fitted-full | 184x112 | 34.7 ms | 5.9 ms (10.6) | 1.0 ms | 1.7 ms | 8.7 ms | 29 fps | 46 fps |
| sponza-fitted-full | 184x224 | 50.4 ms | 12.1 ms (21.6) | 2.0 ms | 2.7 ms | 16.8 ms | 20 fps | 30 fps |
| sponza-fitted-full | 184x179 | 46.0 ms | 9.6 ms (17.1) | 1.6 ms | 2.3 ms | 13.6 ms | 22 fps | 34 fps |
| sponza-fitted-full | 184x149 | 42.9 ms | 8.0 ms (14.2) | 1.3 ms | 2.1 ms | 11.4 ms | 23 fps | 37 fps |
| sponza-fitted-full | 184x112 | 39.0 ms | 6.0 ms (10.7) | 1.0 ms | 1.8 ms | 8.7 ms | 26 fps | 42 fps |
<!-- /generated: framegen-board -->

- A generated frame costs 17 ms at 184x224 and 9 ms at 184x112, and showing
  one between rendered frames raises the displayed rate by 40-60% at every size.
- The warp is the per-pixel maths of the
  [motion-vector attachment](Mesh-Rendering.md#motion-vectors)'s resolve
  plus the splat: about 124 cycles per pixel on one core, and both cores take 57% of one core's time.
- Moving the warp's target out of PSRAM, or a fixed-point warp, are what
  would cut it further; the fill could split over both cores too.
- A generated frame has to reach the panel halfway through the next render,
  while both cores draw it: that slot is the next step's problem.

## Moving objects

The warp moves everything with the camera, so an object moving on its own
(the capybara) lands where it was. What it would need, not built:

- A rigid object: one map per moved instance, which the
  [motion-vector attachment](Mesh-Rendering.md#motion-vectors) already builds from the previous transform, taking only its own pixels.
- A skinned one: a map per bone. Each triangle writes its dominant bone to
  a tag plane; each pixel moves by that bone's change over the step, read from the clip at the later time.
- Tags written only for the moving instances keep the cost to the object's
  pixels, not the whole frame's.

## Verdict

Frame generation pays on the board: a generated frame costs a quarter to
two fifths of a rendered one, its picture is closer to the true frame than
repeating the last one, and Bistro shows 33 fps at 184x224 where it renders 23.
What is left is presenting it mid-render, and moving objects.

## Reproducing

[`launcher/tools/r3d/framegen/`](../../launcher/tools/r3d/framegen/) holds the experiment, removable in one pass
with the host scene renderer's `--dump` option:

| File | Role |
|---|---|
| [`warp.py`](../../launcher/tools/r3d/framegen/warp.py) | The warp and the two fills, on a raster dump |
| [`framegen_score.py`](../../launcher/tools/r3d/framegen/framegen_score.py) | One scene and size: render, warp, score; stills, clip and video |
| [`framegen_report.py`](../../launcher/tools/r3d/framegen/framegen_report.py) | The table and the plot from the scores |
| [`framegen_docs.sh`](../../launcher/tools/r3d/framegen/framegen_docs.sh) | Regenerates this page's table and images |
