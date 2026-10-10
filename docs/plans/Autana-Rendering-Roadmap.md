# Autana: a rendering roadmap for the ESP32-S3

**Status**: proposal. The target games are a gyro-and-buttons FPS, a
rolling-ball game with physics and lighting, and a platformer with parallax
and 2D lighting. The renderer and scene runtime are documented in
[Mesh Rendering](../render/Mesh-Rendering.md) and
[Scene Files](../render/Scene-Files.md). This plan describes future work;
the source tree and those references define the available runtime.

## Runtime foundations

The shell owns the frame loop and app switching. Apps draw and return;
gfx owns the retained framebuffer, band buffers, dirty tracking and panel
transfer. See [Firmware Architecture](../Firmware-Architecture.md) and
[Gfx and Presentation](../Gfx-and-Presentation.md).

The mesh runtime uses baked lighting, span rasterization, clipping and
visibility data. Scene loading, camera clips, dynamic resolution and the
context views share `render_view_t` with the line path. Lines use
`render/r3d_project_x.h` for fixed-point projection; the float projection
helpers in `render/r3d_project.h` provide its test reference and the
camera-space model matrix.

Bulk memory comes from the [app arena](../Building-an-App.md#app-memory)
at entry. Internal SRAM is the scarce resource for hot buffers; PSRAM can
hold retained pictures and content. Buffer placement must be judged from
the final device image, including the diagnostics build's suite memory.

## Measurement and performance work

Measure simulation, drawing and presentation separately through
[`frame_cost`](../tools/Frame-Cost.md). The Sponza and raster-scale suites
under `launcher/main/apps/render_lab/tests/` measure the mesh path and
dynamic-resolution policies. Device results need the image's build id and
the capture that produced them. Host timings do not establish target cost.

Evaluate work per covered pixel and bytes sent per frame rather than
triangles per second. Lower render resolution reduces fill and buffer cost;
the panel still receives physical pixels, so upscaling alone does not
reduce transfer bytes. Dirty regions and a fixed camera can reduce both.

Further fill-rate work should consider affine texture spans, indexed
colormap lighting, band-aware binning and ordering tables where geometry
permits painter's order. Perspective correction can be evaluated at span
intervals with a host reference and visual comparisons. Texture layout
should match access: column-major for vertical spans, row-major for
horizontal spans.

Verify [compiler decisions](../notes/Flashing-and-Toolchain.md#verify-compiler-decisions)
with disassembly. Keep hot loops within the configured instruction cache,
check divide widths and signed rounding, and measure the stripped shipping
image. Measurement switches that skip real work belong only in probes.

## Automated regression gates

Add a build-time internal-RAM gate that budgets static storage, stacks and
the largest hot allocation together for every build variant. A change that
exceeds the budget must fail the build rather than rely on manual heap
measurements.

Add an inlining-cliff gate over the final image's disassembly: flag hot
loops whose helper calls or code growth cross the instruction-cache budget.
Keep this gate active throughout rendering work, with device timings to
validate compiler changes.

## FPS with gyro and buttons

Build a grid-map raycaster first. Walk one integer DDA ray per screen
column, then fill textured vertical spans. Keep per-column depth for
distance-sorted billboard sprites. Begin with flat floor and ceiling
colours plus a dithered distance gradient; add floor texturing only after
measuring its cost.

Gyro look uses `input/tilt.h`; buttons move and act. Touch can supply an
on-screen stick if needed. Cache each column's hit distance, texture
column and span bounds once per frame so band rendering only fills rows.
A sector renderer with varying floor heights is a possible extension.

The proposed acceptance target is a playable wall-and-sprite scene at
60 fps, subject to device measurement. A raycaster is future work and has
no runtime module in this tree.

## Rolling ball with physics and lighting

Use a heightfield or tiled floor with vertex lighting and affine textures.
A regular grid can use painter's order until non-grid objects require
depth testing. A Mode-7 floor with sprite obstacles is another possible
prototype when height variation is unnecessary.

Draw the ball as a lit disc using baked normals or sprites for light
buckets, with a dithered ellipse for its shadow. Fixed-point 2.5D physics
uses height gradients, grid collision and the tilt library's gravity.
Directional and point lighting are per-vertex setup costs.

Choose the camera model before the level format: fixed or stepped cameras
allow the ball and shadow's changed region to be redrawn, while a smooth
follow camera makes the whole picture change. The proposed acceptance
target is 30 fps or better with stable physics across frame durations.

## Platformer with parallax and 2D lighting

Keep two world models available. The first prototype uses the sand
automaton in fixed-camera rooms; a scrolling tile engine remains an option.
Parallax means layers moving at different rates, rather than per-pixel
parallax mapping.

For the automaton world, author levels as ordered rectangles of materials
in cell units, followed by entity spawns and event records. A later rectangle
overwrites an earlier one, including carving with empty material. A host
editor should preview through the real simulation and bake declarative
data. Materials and reactions reference fixed C behaviours, with no script
interpreter per cell.

Entities are sprites over the grid. Collision queries material cells;
fast motion sweeps the intervening cells. Sleeping rooms can be compressed
and swapped in. Sprites draw after the simulation and mark their changed
rectangles dirty. Per-block lighting can feed the palette lookup, with
dithered radial gradients and optional normal lighting within a light's
radius. The proposed target is at least 30 fps with the simulation live,
and no panel transfer when the picture is unchanged.

For the tile world, use layered maps, sprite collision and per-row layer
offsets for line scroll, water and shimmer. Scrolling needs full redraws;
stream tile rows through gfx's band mode. Tile-scale light maps and
colormaps provide the base lighting. The proposed target is 60 fps with
layered scrolling, subject to device measurements.

## Order and acceptance

1. Record device frame-stage costs and memory placement for the shipping
   mesh path. Re-peg performance budgets on the measured image.
2. Extend existing renderer owners for the span, texture and lighting work
   a concrete game needs. Validate clipping and fill rules against host
   references, then measure on the device.
3. Build the FPS raycaster and play-test gyro and button control.
4. Build the rolling-ball prototype after choosing its camera model.
5. Build the room-based platformer, then evaluate scrolling tiles.

A system setting that caps an app's requested resolution is future work;
see the [Settings App Plan](Settings-App-Plan.md). Select band height through
`LAUNCHER_GFX_BAND_HEIGHT` and compare present cost, draw cost and available
RAM when a renderer needs that layout. Keep content, animation, meshlets
and view tables with their existing owners rather than duplicating them
inside a game.

Float32 is appropriate for object transforms and setup. Per-pixel and
per-cell hot loops, and bit-exact host/device behaviour, use integer or
fixed-point arithmetic. Avoid software-emulated double and wide division
in hot loops. SIMD needs a scalar reference asserting identical output.

New shared code needs another consumer and a reference test. Portable logic
is host-tested; device timing is guarded with `DEVICE_BUILD`. Rendering
claims need comparison sheets, crops, video or heatmaps from the existing
[render harness](../tools/Render-Harness.md), alongside sourced measurements.

## Related

- [Board and Memory](../notes/Board-and-Memory.md): memory and cache limits.
- [Display and Rendering](../notes/Display-and-Rendering.md): panel transfer constraints.
- [Building a Scene](../render/Building-a-Scene.md): scene authoring and runtime setup.
