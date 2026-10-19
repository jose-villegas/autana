# Mesh Rendering

How the runtime draws a baked lit-mesh: the frame view, culling, the rasterizer, the
two-core frame and the view modes. The mesh's format and how it is made are
in [Mesh-Import.md](Mesh-Import.md).

`launcher/main/render/` is the engine's 3D layer: the frame view, projection, a
span rasterizer, and a pipeline that draws a mesh whose light was baked
offline. It sits above `gfx/`, whose render targets it draws into, and
otherwise includes `core/`, `math/` and `asset/` (`r3d_lit_mesh.h` reads its
mesh from a pack), so boot and apps both call it. It
draws into buffers its caller hands it, and a framebuffer is only one of
them. The layers are in [Firmware-Architecture.md](../Firmware-Architecture.md).

## What a scene uses

A scene that draws a baked mesh includes `render/r3d_lit_mesh.h`,
`render/raster.h` and `display/viewport.h`;
one that projects points and segments takes `render/r3d_project_x.h`.
Both use `render_view_t` for the per-frame perspective fit and picture basis.

| Noun | What it is |
|---|---|
| `r3d_lit_mesh_t` | A mesh whose light is baked into its colours, made offline ([Mesh-Import.md](Mesh-Import.md)); a view of arrays that stay in the asset pack ([Mesh-Import.md](Mesh-Import.md#the-baked-mesh)) |
| `render_view_t` | One frame: eye, picture axes and forward in world units, pixels per unit depth, centre, near plane and picture size |
| `viewport_t` | The picture's size and the quarter turn the panel is read at; `render_view_make()` and `render_view_refit()` take one |
| `r3d_instance_t` | One mesh and, optionally, its baked placement: a 3x3 (rotation times a positive scale) and a position. No placement draws the mesh as it is |
| `raster_t` | The `r3d_instance_t` array it draws (one mesh is a count of one), at one size, into a scratch block the caller hands it. Its options are fields the caller sets: `clear`, and `upscaled` with a destination picture at least as large |
| `raster_draw()` | Takes a `const render_view_t*` and draws every instance through that frame view |
| `raster_census()` | `raster_draw()`'s cull alone, into the scratch block's list: on an upscaled raster with unchanged destination dimensions it holds at any render size, so a caller can price sizes first ([Dynamic-Resolution.md](Dynamic-Resolution.md)) |
| `raster_draw_culled()` | `raster_draw()` from that list at the raster's size now, without culling again |
| `raster_upscale()` | Nearest-neighbour scales what was drawn up into `destination`; its retained maps change only when either size changes |
| `r3d_span_triangle()` | A scene that projects its own triangles fills them with this, into a window of rows of a render target holding colour and depth, from `render/r3d_span.h` |
| `raster_attachment_t` | A per-pixel map the raster draws beside colour and depth ([Attachments](#attachments)) |
| `raster_show()` | Development builds: the attached [view](#view-modes) paints the colour before the upscale |

Flat or smooth shading is the mesh's own, not an option: a mesh baked flat
carries a colour per face and the raster draws what the mesh carries.

`render_view_make()` builds the frame view from a pose and a lens: it folds
in the panel's quarter turn, fits the lens to the shorter picture side,
ignores the pose's scale and keeps its roll.
Poses follow [math/linear's conventions](../math/Conventions.md#conventions).

Several meshes share one picture: the raster draws each instance in turn
without clearing between, and the depth buffer decides what covers what, so
the order does not matter. An instance's model matrix goes into the lens matrix:
`r3d_lens_place()` composes it with the lens `r3d_lens_init()` built, so
culling, the vertex transform and near clipping all see the mesh where it
sits. The importer bakes the placement from a position, a rotation and a scale
([Scene-Files.md](Scene-Files.md)), so the device does no trigonometry, and an
instance with no model skips the composition and is the unplaced lens
exactly.

## The files

| File | What it is |
|---|---|
| `render_view.h` | The frame view and its pose-and-lens builder |
| `r3d_instance.h` | A mesh and its optional baked placement: what the raster draws |
| `raster.h` | An array of instances drawn on both cores, optionally upscaled into a destination picture |
| `raster_show.h` | Development views paint colour from the raster maps |
| `raster_attachment.h` | What a further attachment declares: its size per pixel, its clear, and the hooks it takes part in a picture with |
| `raster_motion.h` | The motion-vector attachment: where each pixel's point was in the previous picture |
| `context/render_context.h` | The render context: the size and quality a frame is drawn at, apart from what is drawn and from where |
| `resolution/resolution.h` | Dynamic resolution: the steps, the stepped controller and the predictor a render context can opt into |
| `viewport.h` | The viewport, and where a physical pixel lands in the upright picture |
| `r3d_lit_mesh.h` | The baked mesh format: per-vertex or per-face colour, meshlet clusters, a node tree, and the view built from a pack entry |
| `r3d_pipeline.h` | Internal: the raster's stages, lens, cull, transform, draw, and its scratch layout |
| `r3d_span.h` | One depth-tested triangle filled into a window of rows, Gouraud-shaded or face-coloured, its coverage exact on 1/16-pixel positions, and the span writer a further attachment fills through |
| `r3d_project.h` | Float pixel snapping through a `projectf_t` lens and math near clipping |
| `r3d_project_x.h` | Camera-space model matrix and fixed-point pixel projection, with math near clipping |

Only
`r3d_pipeline.h` and `r3d_span_internal.h` are internal: render/ and any
suite or host tool include them.

## The maths

`math/linear/projectf.h` owns the pinhole lens value, continuous float
projection and float near clipping; `math/linear/projectx.h` owns fixed
near clipping at any common camera-space scale. `render_view_lens()` copies
the view's isotropic focal length and optical centre into `projectf_t`.
Render retains pixel snapping, the fixed line path's camera units and its
32-bit per-point divide followed by the pixel-scale multiply. The fixed
path truncates each quotient toward zero. The float reference in
`render/r3d_project.h` biases each pixel offset slightly away from zero,
then truncates toward zero.

Line projection, the animation tracks and the raster all
take their types from `math/linear/`, documented in
[linear maths](../math/Linear.md) and [projection](../math/Projection.md): the raster's lens and motion maps are
`mat4f_t`, composed with `mat4f_mul_affine()`, inverted with
`mat4f_invert_affine()` and applied to each vertex with `mat4f_apply()`.

## One frame

```mermaid
flowchart TB
    View["render_view_t<br/><i>basis, fit, picture</i>"] --> Census
    subgraph Census["raster_census()"]
        direction LR
        Picture["r3d_lens_init()<br/><i>for the picture</i><br/>r3d_lens_place()<br/><i>per instance</i>"] --> Cull
        Cull["r3d_pipeline_cull()<br/><i>walk the tree, nearest first</i>"]
    end
    Census --> List["the scratch block's culled list<br/><i>fixed offset across render sizes</i>"]
    View --> Draw
    Size["the render size"] --> Draw
    List --> Draw
    subgraph Draw["raster_draw_culled()"]
        direction LR
        Fit["r3d_lens_init(), r3d_lens_fit(), r3d_lens_place()<br/><i>per instance, fitted to the render size</i>"] --> Transform["r3d_pipeline_transform()<br/><i>each vertex once</i>"]
        Transform --> DrawStage["r3d_pipeline_draw()<br/><i>near clip, r3d_span</i>"]
    end
    Draw --> Upscale["raster_upscale()<br/><i>into the destination</i>"]
```

A caller builds a frame view and passes its `const render_view_t*` to
`raster_draw()`, which culls and draws each instance in turn with one fitted lens shared by culling and drawing,
and `raster_upscale()` with the destination width and height to compose
that picture. A caller that picks the render size from what culling kept, as the render
context does, calls `raster_census()`, sets the size, then
`raster_draw_culled()` with the same `const render_view_t*`. The stages inside
are `r3d_pipeline.h`'s, for a suite or tool that schedules them itself.

Culling and clipping use caller-owned workspace sized by
`r3d_pipeline_work_bytes()`. The raster's arena, sized by
`raster_scratch_bytes()`, holds one workspace per row slice. Culling reuses
the first workspace before drawing starts. Direct pipeline callers supply
the workspace explicitly; simultaneous draws need distinct blocks.

The stages are split so two cores can share them. Transforming disjoint
cluster lists writes disjoint vertex ranges, and drawing touches only the
rows of its own target. `r3d_pipeline_transform()` also records the screen rows
each cluster spans, so a core drawing half the rows skips a cluster wholly
outside them. The raster draws at the caller's width and height and can
upscale the result into a destination picture, so rendering at half the
panel's size quarters the pixels and halves the rows and spans. The lens
frames the destination's shape and `r3d_lens_fit()` then scales each axis to
the render size on its own, so a render may cut its height more than its
width and still show the same view.

### The render context

A frame view is perspective only. What a frame is drawn at belongs to the render
context (`context/render_context.h`): it owns the raster and its scratch
block, the render size and the debug view. A caller hands it instances, a
frame view and a clear colour, then a destination to upscale into. The scene
manager draws the active camera through the engine's one context,
`render_context_main()`, released when an app exits.

| Call | Meaning |
|---|---|
| `render_context_set_scale()` | the share of the destination each axis draws at; half until set |
| `render_context_set_dynamic_resolution(config, model, step)` | opt-in: each frame draws at a step of `config` to hold its budget ([Dynamic-Resolution.md](Dynamic-Resolution.md)); NULL returns to the fixed scale |
| `render_context_set_debug_view()` | a [view mode](#view-modes), development builds only |
| `render_context_frame()` | the last frame: its step, size, what culling kept, and what its draw and upscale cost |

### On both cores

The work before the framebuffer runs in the app's `update()`, overlapped
with sending the previous frame. Each stage is split between the two cores,
core 1's half dispatched through `core/job.h`. It runs inline when core 1
is busy, and always on a host.

```mermaid
sequenceDiagram
    participant C0 as core 0, shell and app
    participant J as core 1 job worker
    participant P as present on core 1
    C0->>P: gfx_present_begin() sends frame N-1
    Note over C0: update(), cull every cluster
    C0->>J: transform the clusters past the midpoint of the vertex work
    Note over C0: transform the clusters before it
    J-->>C0: job_wait()
    Note over C0: pick the row that halves the estimated draw cost
    C0->>J: clear depth and draw the rows above it
    Note over C0: clear depth and draw the rows below it
    J-->>C0: job_wait()
    C0->>P: gfx_present_wait()
    Note over C0: frame()
    C0->>J: upscale the top half into the framebuffer
    Note over C0: upscale the bottom half
    J-->>C0: job_wait()
    Note over C0,P: frame N is presented on the next pass
```

### View modes

Development builds select one row from the `render_debug_view_t` table in
`render/context/render_context.c`: depth, tiles, motion or meshlets.
`render_context_set_debug_view()` attaches that row and owns its zeroed PSRAM
state.
Switching frees the previous state; `RENDER_DEBUG_VIEW_SHADED` detaches it, and
`render_context_release()` frees it. `render_context_debug_view()` exposes the
row's name and constructor to tools.

`raster_show()` runs the attached view's `show` hook after drawing and before
`raster_upscale()`. It repaints colour while leaving depth intact.

```mermaid
flowchart LR
    Render["the draw<br/><i>colour and depth</i>"] --> Show
    Show["raster_show()<br/><i>attached view paints colour</i>"] --> Upscale["raster_upscale()<br/><i>into the destination</i>"]
```

| View | The colour buffer becomes |
|---|---|
| shaded | untouched: the baked colours as drawn |
| depth | the depth as a grey ramp, nearest white and farthest black |
| tiles | the same ramp, each `RASTER_SHOW_TILE` square at its farthest depth |
| motion | mid-grey for none, red for x and green for y |
| meshlets | a flat hue for each cluster, with disjoint IDs for each instance |

The ramp is stretched over the range this frame drew, so it shows the most
detail within a frame and is not comparable between frames. A pixel nothing
drew takes the raster's `clear`, the colour upscaling gives it, so it reads as empty
in every view; a tile holding one such pixel is empty. The views are at the
raster's own size, before upscaling. `r3d_span.h` defines the depth encoding
they read.

The depth and depth-tile views along a flythrough:

![The depth view along a flythrough](../images/render/sponza-depth.gif)
![The depth-tile view along a flythrough](../images/render/sponza-tiles.gif)

## Attachments

A raster's picture is a render target
([Gfx-and-Presentation.md](../Gfx-and-Presentation.md#render-targets)):
colour and depth, then any further attachment the caller lists in
`raster_t.attachments`. An attachment with pixels is carved from the scratch block at
the drawn size, so a picture drawn at a new size carves anew, and none keeps
pixels from one picture to the next. A view with `bytes_per_pixel == 0`
reserves no scratch pixels. It is not cleared when its `clear` hook is NULL;
depth and tiles have NULL clear hooks and use only colour and depth in their `show` hooks.

```mermaid
flowchart LR
    Begin["begin<br/><i>once per picture</i>"] --> Draw["draw each instance<br/><i>colour, depth, and the span writer<br/>an attachment chose for it</i>"]
    Draw --> Resolve["resolve<br/><i>both cores, disjoint rows</i>"]
    Resolve --> Show["raster_show()"] --> Upscale["raster_upscale()"]
```

| Hook | When | What it may do |
|---|---|---|
| `clear` | the first instance of a picture, on its rows | start its pixels; the only hook colour and depth have |
| `begin` | once per `raster_draw()`, before anything is drawn | read the `const render_view_t*` and the instances, keep its own state |
| `writer` | once per instance | return a span writer, or none. The fill calls every writer after each span's colour and depth, with the span's depth, so each writes where that triangle won. With none, the fill runs exactly as without attachments, and the writers' code sits apart from it |
| `resolve` | once every instance is drawn | turn what was written into the final map |
| `show` | `raster_show()` | paint the colour from it |

A writer finds the pixels its triangle won by their depth equalling the
triangle's, so where two instances meet at exactly the same depth the pixel
takes the later one's write. A writer sees depth and one value per instance, or its base plus the
mesh cluster index when `r3d_span_writer_t.per_cluster` is true; a map that needs more, such as normals, rebuilds it from depth in
its `resolve`.

### Motion vectors

`raster_motion.h`: for each pixel, the previous position of the point it
shows minus this one, two signed bytes in half pixels of this picture.
`RASTER_MOTION_UNKNOWN` marks a pixel nothing drew, the first picture, a
point behind the previous camera, and motion past the range.

The motion view along a flythrough, red for x and green for y:

![The motion-vector view along a flythrough](../images/render/sponza-motion-vectors.gif)

- **The camera.** `begin` builds this picture's lens and the previous
  camera's lens at this picture's size. `resolve` takes each pixel back to
  the world through its depth and projects it through the previous lens. So
  a size change between pictures needs nothing special.
- **Moving instances.** An instance is known by its placement's address. One
  whose placement changed since the previous picture gets its own map, which
  carries the point to where the mesh was. From the first instance that
  moved on, every triangle writes its instance's tag (0 for anything still),
  and `resolve` picks the map by it; before it, pixels keep the 0 they were
  cleared to. On a picture where nothing moved, nothing is written while
  drawing. A raster with motion attached draws at most
  `RASTER_MOTION_INSTANCES_MAX` instances.
- **The caller** owns a zeroed `raster_motion_t`, attaches
  `raster_motion_view()`, and calls `raster_motion_forget()` after a
  cut. The render context attaches it while the motion view is selected.

### Meshlet IDs

`raster_meshlets_view()` clears a 16-bit map to zero and resets its next ID
to one before each draw. Its writer reserves each instance's cluster count
and writes the reserved base plus the mesh's cluster index. The `show` hook
maps IDs to hues through a multiplicative hash; zero takes the clear colour.
IDs are distinct per draw for up to 65535 clusters.
It needs no resolve pass.

The meshlets view along the same flythrough, one hue per cluster:

![The meshlets view along a flythrough](../images/render/sponza-meshlets.gif)

## Coverage and small triangles

A pixel belongs to a triangle when its centre is inside by the top-left
rule, decided in integers on positions snapped to 1/16 pixel. Two
triangles sharing an edge therefore never both fill a pixel, nor both miss
one, in any window of rows. `r3d_pipeline_transform()` snaps each vertex once,
into an 8-byte `r3d_pipeline_vertex_t`. Every position the rasterizer takes is
within 1024 pixels of the origin, which keeps its edge arithmetic in 32
bits: a triangle with a corner behind the near plane or farther off
screen is rebuilt from the mesh and clipped to a guard band inside that
reach, and the fast path stops short of the guard band, so a clip never
cuts an edge that a fast triangle shares.

A detailed mesh drawn small has many triangles covering a few pixel
centres or none, so the draw routes each by the centres its bounding box
holds:

| Bounding box holds | What the draw does |
|---|---|
| no centre | nothing: dropped before its colours are read |
| at most the small-fill side (default 4 × 4 centres) | tests each centre against its three edges, one depth and colour for all |
| more | walks its rows, each edge's column found exactly by an integer step |

Both paths apply the same rule to the same integers, so a small triangle
and a walked one sharing an edge still meet without a gap or an overlap.
The per-centre path skips the walk's setup but loses the triangle's
gradient: at this size that does not show, and larger it shows as facets
on the walls, so the default bound is where cost and look meet (`SMALL_MAX_SIDE`, `r3d_span.c`);
[Small-fill levers](#small-fill-levers) moves it.
`suite_r3d_lit.c` holds every triangle to a reference implementation of the
rule, and a mesh of mixed sizes to one fill per pixel.

A triangle to be walked works out its depth plane first. When every pixel
of its bounding box already holds depth at least as near as the plane's
nearest corner, it could write nothing, and it is dropped before its colour
planes and its rows. The test pays because clusters are drawn roughly
nearest first, so a triangle behind is usually drawn after what hides it.

A pixel centre just outside a triangle extrapolates its planes past the
vertex range, so each span clamps its ends. A kept triangle whose planes
stay inside their ranges at every centre of its bounding box, most of them,
fills its spans without those clamps, which would change nothing there.

### Small-fill levers

Defaults keep the standard look. The flat bake uses one face colour per triangle,
the fully flat end of this range.
Set them with `--set` ([Render-Harness](../tools/Render-Harness.md)).

<!-- generated: small-fill-levers sha256=a0b778d320a8b267f45708e5a20402005fb7de6b32591c51265996b19741b946 -->
| Axis | Tunable | Range | Default | Effect |
|---|---|---|---|---|
| dither | `r3d_span.small_dither` | 0..8 | 0 | Ordered edge coverage in eighths above side 4; 0 disables it |
| dither | `r3d_span.small_dither_cell_bits` | 1..2 | 2 | Log2 of the ordered pattern side in pixels |
| dither | `r3d_span.small_dither_ramp` | 0..1 | 0 | Ramp coverage from 0 at side 4 to the chosen coverage |
| gate | `r3d_span.small_flat_spread_far` | 0..64 | 64 | Far gate: maximum RGB565 channel spread at the size limit; 64 is off |
| gate | `r3d_span.small_flat_spread_near` | 0..64 | 64 | Near gate: spread limit from side 5; interpolated to the far gate; 64 is off |
| size | `r3d_span.small_max_side` | 0..16 | 4 | Bounding-box centres per side eligible for flat fill; 0 disables it |
<!-- /generated: small-fill-levers -->

Live small-fill limits resolve before row workers run. Small dither triangles
test coverage and choose vertex colours in one pixel loop.

The gate measures the largest RGB565 channel spread across the vertices.
For sides above side 4, its limit interpolates from the near gate at side 5
to the far gate at the size limit.
The gate and dither share the triangle's packed vertex colours.

The RGB565 gate's quantization is visible against a floating-channel reference.
This frame uses size limit 8, near gate off and far gate 4.

[![RGB565 gate and floating-channel reference](../images/render/sponza-small-fill-integer-gate.png)](../images/render/sponza-small-fill-integer-gate.crops.png)

<table>
<tr><th width="50%">Technique</th><th width="50%">Diff</th></tr>
<tr><th colspan="2">Today (defaults)</th></tr>
<tr><td width="50%"><img src="../images/render/sponza-small-fill-defaults.gif" width="100%" alt="Defaults"></td><td width="50%"></td></tr>
<tr><th colspan="2">Flat ≤8</th></tr>
<tr><td width="50%"><img src="../images/render/sponza-small-fill-flat8.gif" width="100%" alt="Flat ≤8"></td><td width="50%"><img src="../images/render/sponza-small-fill-flat8-diff.gif" width="100%" alt="Flat ≤8 vs defaults, difference x8"></td></tr>
<tr><th colspan="2">Flat ≤8 gate ramp</th></tr>
<tr><td width="50%"><img src="../images/render/sponza-small-fill-gate.gif" width="100%" alt="Gate ramp"></td><td width="50%"><img src="../images/render/sponza-small-fill-gate-diff.gif" width="100%" alt="Gate ramp vs defaults, difference x8"></td></tr>
<tr><th colspan="2">Flat ≤8 + dither 4 ramp 1</th></tr>
<tr><td width="50%"><img src="../images/render/sponza-small-fill-dither.gif" width="100%" alt="Ramped dither"></td><td width="50%"><img src="../images/render/sponza-small-fill-dither-diff.gif" width="100%" alt="Dither 4 ramp 1 vs defaults, difference x8"></td></tr>
<tr><th colspan="2">Flat bake (fitted)</th></tr>
<tr><td width="50%"><img src="../images/render/sponza-small-fill-bake.gif" width="100%" alt="Flat bake"></td><td width="50%"><img src="../images/render/sponza-small-fill-bake-diff.gif" width="100%" alt="Flat bake vs defaults, difference x8"></td></tr>
</table>

Clips show full renderer frames for 5.5 seconds from source frame 1 at 6 fps,
with a per-GIF full palette and no dithering or scaling.

Unspecified tunables keep their defaults. The gate ramp uses the near gate off,
far 4, and size limit 8. Dither uses coverage 4, ramp 1, and the same size limit.

Each heatmap shows absolute RGB difference x8 from defaults over the same full frames
and window. The [temporal method](../tools/Render-Harness.md#temporal-video-columns) measures
triangle-scale pops; judge dither by eye because it reads as noise here.

<!-- generated: small-fill-temporal sha256=5903d6c98c81ff8bd3e21502700e09a9b8c10474e4eed07458965a25350bd9a4 -->
| Look vs defaults | Extra frame-to-frame change (levels of 255) | Blocks popping (>8/255) |
|---|---|---|
| Defaults | 0.000 | 0.0000% |
| Flat <=8 | 0.510 | 1.0178% |
| Gate ramp | 0.382 | 0.6262% |
| Dither 4, ramp 1 | 0.597 | 1.4627% |
| Flat bake | 3.447 | 10.5584% |
<!-- /generated: small-fill-temporal -->

#### Board frame cost

The table covers the fitted-full raster frame in the Sponza bake suite.
It is generated from the committed [size sweep](data/small-fill/sweep48-sweep.json)
and [look sweep](data/small-fill/s2-sweep.json), with their adjacent plans.

Both use hot-tunable diagnostics builds: the look sweep at `77e546125` and
the size sweep on the main build; the plans record their build ids.
The look-sweep board rows use that commit's tunable names:
`small_flat_spread` and `small_flat_spread_near`.
Measurement with the current names is follow-up work.

<!-- generated: small-fill-board sha256=35796e7f5d24f9c3a271155a33d9bc14ef676375960bd53de16171c1c36b1dbf -->
| Look (size 8 unless defaults) | Local measured frame ms | Capture | Frame vs defaults |
|---|---|---|---|
| Defaults | 48.320 | sweep48, point 0 | +0.00% |
| Flat <=8 | 45.311 | sweep48, point 1 | -6.23% |
| Dither 4, ramp 1 | 48.247 | s2, point 1 | -1.32% |
| Gate 4, near 255 | 46.445 | s2, point 2 | -5.01% |
| Gate + dither 4, ramp 1 | 48.438 | s2, point 3 | -0.93% |
<!-- /generated: small-fill-board -->

- Local frame times are measured on the builds recorded in each plan.
- Chained percentages estimate across builds: look-sweep ratios against flat <=8
  multiply the size sweep's flat <=8/default ratio.

## Memory

The layer allocates nothing, and nothing a frame needs lives at file
scope. A frame's per-vertex and per-cluster buffers, census list, picture
attachments and upscale maps are one block. The list follows the vertex and
cluster buffers, before the picture, at an offset independent of render size.
Its storage is padded to the picture attachments' alignment:
`raster_scratch_bytes()` sizes it, the caller obtains it once and sets
`scratch`, and each call carves it. The caller decides where it lives,
so none of it has to take internal RAM.

## Rules the layer keeps

- **Single precision only.** The FPU has no double, and one stray promotion
  costs an order of magnitude. The firmware build turns
  `-Wdouble-promotion` and `-Wfloat-conversion` into errors for the whole
  `main` component (see [Build-Variants.md](../Build-Variants.md)).
- **Host-testable.** The headers are ESP-IDF-free, and the portable
  `test/suites/suite_r3d_*.c` suites check them on a laptop.
  `suite_r3d_lit.c` builds its meshes inside the test, never a baked one.
- **The caller owns the environment.** Viewport, panel quarter, units and
  timeline are passed in. No code path reads the panel's size.

## Related

- [Firmware-Architecture.md](../Firmware-Architecture.md): the layers and the frame loop
- [Gfx-and-Presentation.md](../Gfx-and-Presentation.md#present-who-runs-it): the split present `update()` overlaps
- [Building-an-App.md](../Building-an-App.md#app-memory): the app arena, one place a frame's scratch block can come from
