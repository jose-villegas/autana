# Mesh Rendering

How the runtime draws a baked lit-mesh: the frame view, culling, the rasterizer
and the two-core frame. Which pixels a triangle fills is in
[Coverage.md](frame/Coverage.md), depth and attachments in
[Attachments.md](frame/Attachments.md), the debug views in
[View-Modes.md](frame/View-Modes.md).

The mesh's format is [Baked-Mesh.md](offline/Baked-Mesh.md); how it is made is
[Mesh-Import.md](offline/Mesh-Import.md).

`launcher/main/render/` is the engine's 3D layer: the frame view, projection, a
span rasterizer, and a pipeline that draws a mesh whose light was baked
offline. Boot and apps both call it.

It sits above `gfx/`, whose render targets it draws into, and otherwise
includes `core/`, `math/` and `asset/` (`r3d_lit_mesh.h` reads its mesh from
a pack). The layers are in [firmware/README.md](../firmware/README.md).

It draws into buffers its caller hands it, and a framebuffer is only one of
them.

## What a scene uses

A scene that draws a baked mesh includes `render/r3d_lit_mesh.h`,
`render/raster.h` and `display/viewport.h`; one that projects points and
segments, `render/r3d_project_x.h`. Both take each frame's perspective fit
and picture basis from `render_view_t`.

| Noun | What it is |
|---|---|
| `r3d_lit_mesh_t` | A mesh whose light is baked into its colours, made offline ([Mesh-Import.md](offline/Mesh-Import.md)); a view of arrays that stay in the asset pack ([Baked-Mesh.md](offline/Baked-Mesh.md#clusters-and-the-tree)) |
| `render_view_t` | One frame: eye, picture axes and forward in world units, pixels per unit depth, centre, near plane and picture size |
| `viewport_t` | The picture's size and the quarter turn the panel is read at; `render_view_make()` and `render_view_refit()` take one |
| `r3d_instance_t` | One mesh and, optionally, its baked placement: a 3x3 (rotation times a positive scale) and a position. No placement draws the mesh as it is |
| `raster_t` | The `r3d_instance_t` array it draws (one mesh is a count of one), at one size, into a scratch block the caller hands it. Its options are fields the caller sets: `clear`, and `upscaled` with a destination picture at least as large |
| `raster_draw()` | Takes a `const render_view_t*` and draws every instance through that frame view |
| `raster_census()` | `raster_draw()`'s cull alone, into the scratch block's list: on an upscaled raster with unchanged destination dimensions it holds at any render size, so a caller can price sizes first ([Dynamic-Resolution.md](frame/Dynamic-Resolution.md)) |
| `raster_draw_culled()` | `raster_draw()` from that list at the raster's size now, without culling again |
| `raster_upscale()` | Nearest-neighbour scales what was drawn up into `destination`; its retained maps change only when either size changes |
| `raster_upscale_picture()` | The same through the upscale filter, of another colour and depth picture at the drawn size when given one, marking the seams it blends |
| `r3d_span_triangle()` | A scene that projects its own triangles fills them with this, into a window of rows of a render target holding colour and depth, from `render/r3d_span.h` |
| `raster_attachment_t` | A per-pixel map the raster draws beside colour and depth ([Attachments](frame/Attachments.md#attachments)) |
| `raster_show()` | Development builds: the attached [view](frame/View-Modes.md) paints the colour before the upscale |

Flat or smooth shading is the mesh's own, not an option: a mesh baked flat
carries a colour per face and the raster draws what the mesh carries.

`render_view_make()` builds the frame view from a pose and a lens: it folds
in the panel's quarter turn, fits the lens to the shorter picture side,
ignores the pose's scale and keeps its roll. Poses follow
[math/linear's conventions](../math/Conventions.md#conventions).

Several meshes share one picture: the raster draws each instance in turn
without clearing between, and the depth buffer decides what covers what, so
the order does not matter.

`r3d_lens_place()` composes an instance's model matrix with the lens
`r3d_lens_init()` built, so culling, the vertex transform and near clipping
all see the mesh where it sits. With no model, an instance is the unplaced
lens exactly.

The importer bakes the placement from a position, a rotation and a scale
([Scene-Files.md](scene/Scene-Files.md)), so the device does no trigonometry.

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
| `dynamic/r3d_dynamic_mesh.h` | A lit mesh rebuilt from caller triangles each frame: clusters of `R3D_DYNAMIC_CLUSTER_TRIANGLES` in source order, a tree with at most `R3D_LIT_NODE_CHILDREN_MAX` children per inner node, and vertex and cluster counts held at their capacities so the raster sizes its scratch once |
| `r3d_pipeline.h` | Internal: the raster's stages, lens, cull, transform, draw, and its scratch layout |
| `r3d_span.h` | One depth-tested triangle filled into a window of rows, Gouraud-shaded or face-coloured, its coverage exact on 1/16-pixel positions, and the span writer a further attachment fills through |
| `r3d_project.h` | Float pixel snapping through a `projectf_t` lens and math near clipping |
| `r3d_project_x.h` | Camera-space model matrix and fixed-point pixel projection, with math near clipping |

Only `r3d_pipeline.h`, `raster_internal.h` and `r3d_span_internal.h` are
internal: render/ and any suite or host tool include them.

## The maths

Line projection, the animation tracks and the raster all take their types
from `math/linear/`, documented in [linear maths](../math/Linear.md) and
[projection](../math/Projection.md).

| Owner | What it holds |
|---|---|
| `math/projection/projectf.h` | The pinhole lens value, continuous float projection and float near clipping |
| `projectx_t` | A Q16.16 lens; fixed near clipping accepts any common camera-space scale |
| `render_view_t.lens` | The view's square-pixel pinhole |
| `mat4f_t` | The raster's lens and motion maps: composed with `mat4f_mul_affine()`, inverted with `mat4f_invert_affine()`, applied to each vertex with `mat4f_apply()` |
| render | Pixel snapping and fixed line units: its 32-bit quotient truncates toward zero before the pixel-scale multiply |
| `render/r3d_project.h` | The float reference: biases pixel offsets slightly away from zero before truncating toward zero |

## One frame

```mermaid
flowchart TB
    View["render_view_t<br/><i>basis, fit, picture</i>"] --> Census
    subgraph Census["raster_census()"]
        direction LR
        Picture["r3d_lens_init()<br/><i>for the picture</i><br/>r3d_lens_place()<br/><i>per instance</i>"] --> Cull
        Cull["r3d_pipeline_cull()<br/><i>walk the tree, nearest first;<br/>bound each kept box's depth</i>"] --> Scale
        Scale["inverse_depth_scale_fit()<br/><i>the depth scale</i>"]
    end
    Census --> List["the scratch block's census<br/><i>depth scale, then culled list;<br/>fixed offset across render sizes</i>"]
    View --> Draw
    Size["the render size"] --> Draw
    List --> Draw
    subgraph Draw["raster_draw_culled()"]
        direction LR
        Fit["r3d_lens_init(), r3d_lens_fit(), r3d_lens_place(),<br/>r3d_lens_set_depth_near()<br/><i>per instance, fitted to the render size</i>"] --> Transform["r3d_pipeline_transform()<br/><i>each vertex once</i>"]
        Transform --> DrawStage["r3d_pipeline_draw()<br/><i>near clip, r3d_span</i>"]
    end
    Draw --> Upscale["raster_upscale()<br/><i>into the destination</i>"]
```

A caller builds a frame view and passes its `const render_view_t*`:

| Caller | Calls |
|---|---|
| Draws at a set size | `raster_draw()`, which culls every instance, fits the [depth scale](frame/Attachments.md#depth) to what it kept, then draws each in turn with the fitted lens culling used, then `raster_upscale()` with the destination width and height |
| Picks the render size from what culling kept, as the render context does | `raster_census()`, sets the size, then `raster_draw_culled()` with the same view, then `raster_upscale()` |
| A suite or tool that schedules the stages itself | `r3d_pipeline.h`'s stages |

Culling and clipping use caller-owned workspace sized by
`r3d_pipeline_work_bytes()`:

- The raster's arena, sized by `raster_scratch_bytes()`, holds one workspace
  per row slice; culling reuses the first before drawing starts.
- Direct pipeline callers supply the workspace explicitly; simultaneous draws
  need distinct blocks.

The stages are split so two cores can share them:

- Transforming disjoint cluster lists writes disjoint vertex ranges.
- Drawing touches only the rows of its own target.
  `r3d_pipeline_transform()` records the screen rows each cluster spans, so a
  core drawing half the rows skips a cluster wholly outside them.

The raster draws at the caller's width and height and can upscale into a
destination picture: rendering at half the panel's size quarters the pixels
and halves the rows and spans.

The lens frames the destination's shape, and `r3d_lens_fit()` scales each
axis to the render size on its own, so a render may cut its height more than
its width and still show the same view.

### The render context

A frame view is perspective only. What a frame is drawn at belongs to the
render context (`context/render_context.h`): it owns the raster and its
scratch block, the render size and the debug view.

A caller hands it instances, a frame view and a clear colour, then a
destination to upscale into. The scene manager draws the active camera
through the engine's one context, `render_context_main()`, released when an
app exits.

| Call | Meaning |
|---|---|
| `render_context_set_scale()` | the share of the destination each axis draws at; half until set |
| `render_context_set_dynamic_resolution(config, model, step)` | opt-in: each frame draws at a step of `config` to hold its budget ([Dynamic-Resolution.md](frame/Dynamic-Resolution.md)); NULL returns to the fixed scale |
| `render_context_set_frame_generation(on)` | a drawn frame may be followed by one warped from it instead of drawn; when, and what draws instead: [Frame-Generation.md](frame/Frame-Generation.md#for-an-app) |
| `render_context_set_debug_view()` | a [view mode](frame/View-Modes.md), development builds only |
| `render_context_frame()` | the last frame: its step, size, what culling kept, and what its draw and upscale cost |

### On both cores

The work before the framebuffer runs in the app's `update()`, overlapped
with sending the previous frame. Each stage is split between the two cores:

- Core 1's half is dispatched through `core/job.h`. It runs inline when
  core 1 is busy, and always on a host.
- Present shares core 1, so `job_run_steered()` times both halves of every
  stage and moves work toward the faster core.

```mermaid
sequenceDiagram
    participant C0 as core 0, shell and app
    participant J as core 1 job worker
    participant P as present on core 1
    C0->>P: gfx_present_begin() sends frame N-1
    Note over C0: update(), cull every cluster
    C0->>J: transform the clusters past the instance's transform share
    Note over C0: transform the clusters before it
    J-->>C0: job_wait()
    Note over C0: pick the row at the instance's top share
    C0->>J: clear depth and draw the rows above it
    Note over C0: clear depth and draw the rows below it
    J-->>C0: job_wait()
    Note over C0: job_share_update() steers each share by both halves' times
    C0->>P: gfx_present_wait()
    Note over C0: frame()
    C0->>J: upscale the top rows at the upscale share
    Note over C0: upscale the rows below them
    J-->>C0: job_wait()
    Note over C0,P: frame N is presented on the next pass
```

`raster_split_t` holds every raster share: caller-owned, NULL = even,
forget after a cut. Core 1's time counts from dispatch, so a wait behind
present counts against it.

## Culling

What the cull drops, stage by stage, what it keeps along two camera paths and
what each stage costs: [Culling.md](frame/Culling.md).

## Memory

The layer allocates nothing. Raster draw work owns the resolved camera
settings and per-side spread table, shared read-only by the row workers.

Per-vertex and per-cluster buffers, the census list, picture
[attachments](frame/Attachments.md#attachments) and upscale maps share one block. The census comes first, its depth scale then
its list, at an offset independent of render size; storage is padded for
picture attachments.

`raster_carve()` in `raster_internal.h` states the layout once, and
`raster_scratch_bytes()` sizes the block from it.

The caller obtains the block once and sets `scratch`; each call carves it.
The caller decides where it lives, so it need not consume internal RAM.

## Rules the layer keeps

- **Single precision only.** The FPU has no double, and one stray promotion
  costs an order of magnitude. The firmware build turns
  `-Wdouble-promotion` and `-Wfloat-conversion` into errors for the whole
  `main` component (see [Build-Variants.md](../tools/Build-Variants.md)).
- **Host-testable.** The headers are ESP-IDF-free, and the portable
  `test/suites/suite_r3d_*.c` suites check them on a laptop.
  `suite_r3d_lit.c` builds its meshes inside the test, never a baked one.
- **The caller owns the environment.** Viewport, panel quarter, units and
  timeline are passed in. No code path reads the panel's size.

## Related

- [firmware/README.md](../firmware/README.md): the layers and the frame loop
- [gfx/Present.md](../gfx/Present.md#present-who-runs-it): the split present `update()` overlaps
- [Building-an-App.md](../firmware/Building-an-App.md#app-memory): the app arena, one place a frame's scratch block can come from
