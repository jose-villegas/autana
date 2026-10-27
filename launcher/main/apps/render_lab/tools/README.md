# Render Lab tools

Host-only scripts; the firmware build skips this folder. The render harness
itself is [`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

Before a source bake or reference render, pull the
[mesh source files](../../../../tools/r3d/README.md).

## Host renders

```sh
./launcher/main/apps/render_lab/tools/render_lab_render_host.sh
```

Writes every declared scene under `tools/results/render/render_lab/`:
`sponza-landscape.bmp` is the atrium flythrough. These renders are
`|nopin`: their floating-point camera paths can reach different pixels
across compilers. HUD renders also format a `double` fps readout.
The fps text comes from the host fixture - time the board with a device
capture.

`render_lab_render --tune NAME=VALUE` applies a console tuning before the scene
enters; repeat it for more settings. `--live-tune NAME=VALUE` applies it after
entry, so the frame hook changes the next frame without reloading the scene.

### Menu

The BOOT menu has a SCENE dropdown of scene families (`family` in
`render_lab_scene_t`), then a VARIANT dropdown when the family has several scenes (Sponza's
bakes, from `variant`), then the rows the showing scene declares in its
`controls` (Capybara's CLIP and BLEND). Order in `scenes[]` is the order of every list, and
index 0 (Sponza Fitted Full) is the scene a start with no key shows. Console
and host scene keys are unchanged.

### Water

The Water scene is a tank the size of the screen and as deep as the device
(`board/board_geometry.h`), filled by `flip3` and tipped by the device's gravity
and shake. The menu's WATER FILL sets the share of the tank's volume, 10 to
90 % in steps of 10.

| Command or tunable | Does |
|---|---|
| `render water fill <percent>` | sets the fill; the water refills on the next frame |
| `render water tour` | plays the scripted tour the GIF below shows, for renders and repeatable timings |
| `water.gravity_mm_s2` | the gravity the water runs under; below Earth's, so a 35 mm tank sloshes at a pace a frame can follow |
| `water.grid_internal` | 1 places the simulation grid in internal RAM, for measuring against PSRAM; read when the scene enters |

Each frame, while the last one goes out to the panel:

| Step | Owner |
|---|---|
| Gravity and shake, in g, box axes | `input/motion/gravity.h` |
| Up to four 8 ms substeps | `physics/fluid/flip3.h` `flip3_step()` |
| The smoothed surface field | `flip3_field()` |
| Its surface, as triangles | `math/geometry/iso_surface.h` `iso_surface_nets()` |
| Shading per vertex, encoded through a table | the scene; `r3d_skin_light_encode()` |
| Clusters and a tree the raster walks | `render/dynamic/r3d_dynamic_mesh.h` |
| Drawn as the water entity | `scene_renderer_set_mesh()` |

![The water scene's scripted tour](../../../../../docs/images/overview/render-lab-water.gif)

### Small fill

`render_lab.small_fill_ceiling`, `render_lab.small_fill_gate` and
`render_lab.small_fill_style` apply to the scene camera each frame.
On a development build the BOOT menu has a SMALL FILL dropdown with every
`SMALL_FILL_` preset; picking one sets the three tunables.

![The Sponza menu closed, then the SCENE, VARIANT and SMALL FILL lists open](images/scene-menu/scene-menu.png)

![The SMALL FILL list open in landscape](images/scene-menu/small-fill-landscape.png)

The generic scene viewer accepts `--small-fill CEILING,GATE,STYLE`; its defaults
come from `SMALL_FILL_SMOOTH`. Styles use the `small_fill_style_t` values.
The [engine feature](../../../../../docs/render/Mesh-Rendering.md#small-fill)
defines the axes and defaults.

```sh
sh launcher/tools/render/scene_viewer.sh launcher/demo/sponza/sponza.scene.toml \
    --object atrium_fitted_full --small-fill 12,16,1 -o grain.bmp
```

The app host accepts `--tune` before entry or `--live-tune` after entry.

### Menu renders

`--menu` presses BOOT on the first frame, so the render shows the scene menu
instead of the scene. `--tap X,Y` adds one press and release there, in panel
pixels: `menu-capybara-clips` opens the clip list. The menu draws over a
solid background with no float on its path, so these renders are pinned in
`render_lab_render_baseline.txt`. Re-pin with `--update-baseline` after
looking at the images.

### Camera presentation

`render_lab.present` selects the scene camera's present mode: NEAREST (0, the
default), SCANLINE (1) or DOUBLE (2), in `camera_present_t` order. Both scenes
apply it every frame, and the perf suites size and compose the same way.

```sh
autana tune render_lab.present 1
autana suite run_sponza_perf_suite --set render_lab.present=1
sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh --build-only -o /tmp/present
/tmp/present/render_lab_render --scene sponza-fitted-full --tune render_lab.present=1 --no-hud -o /tmp/present/scan.bmp
```

Seeded `perf_compare` on one board, eight seeds per side, ms per frame:

| Scene | NEAREST | SCANLINE | Δ |
|---|---:|---:|---:|
| sponza fitted-full | 45.4 | 32.9 | −27.7% |
| sponza full | 43.1 | 31.2 | −27.7% |
| sponza flat | 32.7 | 25.1 | −23.3% |
| sponza lite | 34.8 | 24.3 | −30.0% |
| sponza fitted | 35.2 | 24.8 | −29.5% |
| sponza flat-fitted | 32.9 | 25.4 | −22.7% |
| capybara render (paused) | 6.75 | 3.87 | −42.7% |

A separate scoped A/B on the same day (one flash, two interleaved repeats) timed
all three modes on sponza fitted-full, in ms:

| Quarter | NEAREST | DOUBLE | SCANLINE |
|---|---:|---:|---:|
| portrait | 45.4 | 31.9 | 32.7 |
| landscape | 42.4 | 30.3 | 31.1 |

The lines follow the panel's rows and darken the clear background too. The
engine page shows the [presentation GIFs](../../../../../docs/render/Dynamic-Resolution.md#camera-presentation).

### Debug views

`--view shaded|depth|tiles|motion|meshlets` selects the declared context view.
Depth and tiles show a lit-mesh scene's depth buffer, or that depth
reduced to `RASTER_SHOW_TILE` squares. The depth is `raster_draw()`'s
own buffer, unchanged; `raster_show()` only colours it and takes each
tile's farthest depth, so the views are what the renderer holds at the
moment it upscales the frame. Near is bright and far dark, stretched over the
range that frame drew, so grey compares pixels within a frame, not across
frames; a pixel nothing reached takes the scene's clear colour, as the
shaded frame does, and a tile with one such pixel is empty.

They are ordinary renders: `sponza-depth-*.bmp` and `sponza-tiles-*.bmp`
beside `sponza-*.bmp`, each with a `.png` when Pillow is installed, turned to
the panel's orientation and the size the script declares. The picture is the
renderer's resolution, half the panel's each way, upscaled like the shaded
one. They are `|nopin` too.
`--view` sets the tunable `render_lab.view`, so on a development build
`autana tune render_lab.view N` selects view N: zero is shaded, and the remaining
values follow the render_debug_view_t table order in render/context/render_context.c. `--view` on a
scene with no lit mesh, an unknown name or no value fails the run.
`tests/test_render_views.py` checks the views against the shaded render.
Motion paints offsets red for x and green for y. Meshlets paints each mesh
cluster a flat hue, with disjoint IDs across instances.

![The meshlets view: full, lite and fitted](../../../../../docs/images/render/sponza-meshlets.png)

A pose of a camera path is `--frames` times `--dt`. `--camera NAME`
selects a scene camera; omitted, it draws the first camera.
`--console FRAME:WORDS` runs the board's `render WORDS` command, such as
`clip walk 400`, before frame FRAME draws; repeat it for a sequence.

`dynres_quality.sh WORK OUT.csv CAMERA WxH [WxH ...]` scores each size
against the reference along that camera's clip, at intervals taken from
the script and over the clip's full period. Per-path results belong in
`docs/render/data/dynamic-resolution-quality-CAMERA.csv`.

The tour is authored as camera keys in `launcher/demo/sponza/tour.keys.toml`;
the pack build bakes it, so an edit there needs nothing else.

A host pose can be drawn with:

```sh
render_lab_render --scene sponza --frames 1 --dt 15000 --view depth -o depth.bmp
```

## On the board

A development build answers these from any shell, so a measurement selects
its scene by name rather than through the menu:

| Command | Does |
|---|---|
| `autana render clips` | the showing scene's clips, and which one the picker holds; a scene without a clip picker does not answer |
| `autana render clip <name> [blend_ms]` | plays that clip, blending over blend_ms, which the picker then keeps (its current blend when omitted) |
| `autana render scenes` | every scene's key and name, and which one is showing |
| `autana render scene <key>` | switches to the scene with exactly that key, such as `sponza`, `sponza-lite` or `sponza-flat-fitted` |
| `autana tune render_lab.scale <n>` | the fixed render scale in hundredths of the panel: 200 is half size |
| `autana tune render_lab.budget <ms>` | dynamic resolution on the scene showing; 0 turns it off, as does the menu's DYNAMIC RES |
| `autana tune render_lab.orbit_ms <ms>` | turns the Capybara scene's camera once about the capybara every that many milliseconds; 0 stops it |

The Capybara scene's BOOT menu holds the same clip picker: the CLIP
dropdown plays the chosen clip over the BLEND slider's time.

![The clip picker and the capybara walking, on the board](images/clip-picker/board-picker.png)

A screenshot's state carries an `app` object naming the scene, whether the
menu is open and the scale, so two captures can
be checked to have measured the same thing.

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `render-lab-sponza.gif` | the start of the Sponza flythrough, on the fitted full mesh |
| `render-lab-sponza-flat.gif` | the flythrough with the fitted flat-shaded bake |
| `render-lab-capybara.gif` | every clip the capybara scene lists, in its order and for an equal time, each blended into the next and, more slowly, back to the first, as the camera turns once around it |
| `render-lab-water.gif` | the water scene's scripted tour (`render water tour`): upright, tipped right and left, laid back, flat, toward the viewer, upright, then shaken |

Run the app shots from the repository root with Python, Pillow, numpy and ffmpeg:

```sh
PYTHON=python sh launcher/main/apps/render_lab/tools/doc_images.sh /path/to/out /path/to/work
```

`tests/test_sky_through_walls.py`
checks the app flythrough against its sky-through-wall ceilings.

## Sponza poses

The flythrough is a glTF camera animation, `launcher/demo/sponza/flythrough.glb`,
named by `launcher/demo/sponza/flythrough.anim.toml`. Its poses for
[`report_triangle_sizes.sh`](../../../../tools/r3d/README.md#triangle-sizes)
come from [`tools/anim/track_host.py`](../../../../tools/anim/README.md),
which runs the device's track sampler over the clip, at the poses
`suite_sponza_perf.c` times (every `SPONZA_PERF_POSE_EVERY_MS`) and the size
`sponza_content.h` names and the lens of the scene's camera object
(`launcher/demo/sponza/sponza.scene.toml`):

```sh
pose_every_ms=$(python -c \
    'import sys; sys.path.insert(0, "launcher/tools"); from build.c_header import read_define; print(read_define("launcher/main/apps/render_lab/sponza_content.h", "SPONZA_PERF_POSE_EVERY_MS"))')
python launcher/tools/anim/track_host.py \
    launcher/demo/sponza/flythrough.anim.toml \
    --every "$pose_every_ms" \
    --poses camera 184 224 0.62 6 |
    ./launcher/tools/r3d/report_triangle_sizes.sh \
        --mesh sponza.atrium -
```

## The capybara asset

`launcher/demo/capybara/capybara.blend` is a hand-modelled low-poly capybara
with a control rig and in-place loops at 30 fps: `idle`, `walk`, `walk_fast`,
`gallop` and `half_bound`. It is the source asset for skinned-mesh import;
nothing in the build reads it.

Its glTF export, `capybara.glb`, is a cached bake: deform bones only,
every loop as an animation, four influences per vertex. Host tools that read
glTF use it, such as the
[skinned-mesh lighting](../../../../../docs/render/Skinned-Lighting.md)
measurement; `bake.py path capybara.glb` prints where it is.
`capybara.import.toml` names the
`.blend` and the loops to export (the file also holds the rig's own
`capyrigAction`), and `launcher/tools/bake/bake.py` runs the model-agnostic
exporter in Blender. By hand, it is:

```sh
blender --background --factory-startup --python launcher/tools/gltf/blend_skin_to_glb.py -- \
    launcher/demo/capybara/capybara.blend capybara.glb \
    --clips idle,walk,walk_fast,gallop,half_bound
```

and measure the lighting again:

```sh
launcher/tools/r3d/skin_light/report_skin_light.sh "$(python launcher/tools/bake/bake.py path capybara.glb)" gallop
```

## Meshlet size captures

Use the [meshlet capture recipe](../../../../tools/render/README.md#meshlet-size-captures)
with `--scene launcher/demo/sponza/sponza.scene.toml` and
`--suite run_sponza_perf_suite --size-tune sponza_perf.shipped_meshlet`. Its fixed flythrough samples measure the selected
renderer with both culling settings. The generated table belongs to
[Render Pipeline](../../../../../docs/render/Render-Pipeline.md#meshlets).

## Skinned frame comparison

The model-independent skin-frame tool compares scene-controlled C skinning
with the glTF reference and generates a sheet of two versus four influences.
The selected source vertices are marked red in enlarged crops. All panels use
bind colours, so the comparison isolates deformation rather than lighting.

```sh
python launcher/tools/r3d/skin/skin_frame.py "$(python launcher/tools/bake/bake.py path capybara.glb)" \
  --poses gallop:200 half_bound:200 \
  --vertices 7,11,44,54,77,129,171,205,211,260,302 \
  --out launcher/main/apps/render_lab/tools/images/skin-frame
```

![C skinning beside the glTF reference](images/skin-frame/reference-sheet.png)
![Two and four influence limits, with amplified image differences](images/skin-frame/influence-sheet.png)
![Each requested vertex at both influence limits](images/skin-frame/vertex-crops.png)

The circles locate the corresponding posed vertex in each panel; a crop may
show a vertex through an occluding surface. The image difference is amplified
by the comparison tool. Exact packed-weight reference errors and per-vertex
influence-limit displacements are in
[`measurements.json`](images/skin-frame/measurements.json). This tool bakes its
own LMSH with the meshlet writer, so its vertex layout is independent
of a scene pack's bake. The sheets use the poses named on the command line.

## Perf capture quarters

The render perf suites use the shared `tests/r3d_perf_quarters.h` table.
Every timed render window runs with an explicit portrait or landscape
viewport; full-frame windows hold the display quarter. Rows include
`quarter=portrait` or `quarter=landscape`.
The skin, animate and animation-step windows have no quarter because they
do not render. The frame harness requires a held quarter and its
fixture releases the hold at exit. Raster sizes stay in physical panel
axes; the camera viewport applies the quarter turn.

`report_performance.py` reports each render quarter separately. The skin
budget is `SKIN_PERF_BUDGET_NS_PER_VERTEX` in `tests/skin_perf_log.h`.
Meshlet and reference-quality report readers select portrait for their
matching reference views.

## Frame timing profiles

`run_sponza_perf_suite` measures fixed camera poses at the spacing in
`SPONZA_PERF_POSE_EVERY_MS`. Each frame-cost row reports mean, worst,
median and p95 before presentation, with stage splits at those same poses.
The pixel-equivalence checks in that suite and the camera checks in
`suite_sponza.c` use `SPONZA_POSE_EVERY_MS`.

`run_render_lab_frame_profile_suite` plays each Sponza scene's camera clip
and the capybara gallop for one clip period in each perf quarter. It times
update, render, compose and present with the HUD hidden.

After each playback the suite names its `FRAME_PROFILE_WORST_K` slowest
frames by path time. It then replays poses paused: `FRAME_PROFILE_PATH_POSES`
spread evenly along the path, then each worst frame's.
Each replay draws one frame through the `cull` view for its counts and
`FRAME_PROFILE_REPLAY_DRAWS` shaded frames for their frame-cost report.
A paused frame does not skin again, so capybara's replayed split has no
skinning in it. `report_performance.py` ranks the worst frames with those
splits and counts. Its "why" names the stage that most exceeds the path's
median pose and the count furthest above its median.

The [perf tools](../../../../tools/perf/README.md#real-time-playback)
define playback timing and the `frame_profile:` line format.

Each suite owns a table of mean goals and p95 ceilings per row and quarter.
Zero means unpegged: it logs without enforcing a budget. Board ceilings
are reported without enforcement under QEMU.

<!-- generated: frame-timing-profiles check: python launcher/main/apps/render_lab/tools/frame_profile_tables.py --worst-board 1 --scenes sponza-fitted-full,sponza-fitted,sponza,sponza-lite,sponza-flat,sponza-flat-fitted,capybara --check -->
### Measured frame rate

Cells list board 1 / board 2.
Playback includes update, render, compose and present.

| Scene | Quarter | Mean fps | p50 ms | p95 ms | Worst ms |
|---|---|---:|---:|---:|---:|
| sponza-fitted-full | portrait | 20.7 / 20.9 | 48.7 / 48.5 | 60.2 / 59.8 | 61.6 / 60.6 |
| sponza-fitted-full | landscape | 21.9 / 22.1 | 46.2 / 45.8 | 55.7 / 55.2 | 58.1 / 57.3 |
| sponza-fitted | portrait | 25.0 / 25.2 | 40.5 / 40.2 | 47.4 / 47.0 | 48.4 / 47.9 |
| sponza-fitted | landscape | 26.2 / 26.4 | 38.6 / 38.3 | 44.1 / 43.8 | 45.1 / 44.6 |
| sponza | portrait | 20.6 / 20.8 | 49.1 / 48.8 | 58.8 / 58.3 | 60.5 / 59.5 |
| sponza | landscape | 21.4 / 21.5 | 47.0 / 46.6 | 55.8 / 55.3 | 57.6 / 57.0 |
| sponza-lite | portrait | 24.1 / 24.1 | 42.1 / 42.2 | 47.4 / 47.4 | 48.6 / 48.4 |
| sponza-lite | landscape | 24.9 / 24.8 | 40.6 / 40.8 | 45.6 / 45.6 | 46.5 / 46.3 |
| sponza-flat | portrait | 24.5 / 24.6 | 41.0 / 40.8 | 49.6 / 49.5 | 50.6 / 50.1 |
| sponza-flat | landscape | 24.9 / 25.0 | 40.4 / 40.2 | 48.2 / 48.0 | 49.4 / 48.8 |
| sponza-flat-fitted | portrait | 25.2 / 25.5 | 40.3 / 39.9 | 49.8 / 49.3 | 51.5 / 50.5 |
| sponza-flat-fitted | landscape | 26.1 / 26.4 | 38.8 / 38.5 | 47.4 / 47.0 | 49.4 / 48.3 |
| capybara | portrait | 47.8 / 48.1 | 20.9 / 20.7 | 21.3 / 21.0 | 21.6 / 21.6 |
| capybara | landscape | 44.3 / 46.7 | 22.5 / 21.4 | 23.0 / 21.5 | 23.0 / 21.6 |

### Worst frames along the path

Board 1. Each scene's played frames over path time, the worst ranked,
above what the cull counted at poses replayed paused along the path.

![sponza-fitted-full: frames along the path and the cull's counts](images/worst-frames/sponza-fitted-full.png)
![sponza-fitted: frames along the path and the cull's counts](images/worst-frames/sponza-fitted.png)
![sponza: frames along the path and the cull's counts](images/worst-frames/sponza.png)
![sponza-lite: frames along the path and the cull's counts](images/worst-frames/sponza-lite.png)
![sponza-flat: frames along the path and the cull's counts](images/worst-frames/sponza-flat.png)
![sponza-flat-fitted: frames along the path and the cull's counts](images/worst-frames/sponza-flat-fitted.png)
![capybara: frames along the path and the cull's counts](images/worst-frames/capybara.png)

#### Worst frames, ranked

Played frames ranked by wall time. Stage ms and counts come from replaying each pose paused;
Why compares them with the median of the 64 poses spaced evenly along the path (896 of 896 read).

| Scene | Quarter | Rank | Path s | Frame ms | r3d.cull ms | r3d.transform ms | r3d.draw ms | r3d.upscale ms | Clusters tested / kept / hidden / drawn | Triangles submitted / set up / cover / miss / away | Why |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| sponza | portrait | 1 | 0.07 | 60.5 | 0.68 | 4.89 | 40.92 | 2.50 | 537 / 467 / 105 / 318 | 13927 / 6945 / 2519 / 3856 / 2739 | r3d.draw +7.1 ms; triangles missed ×1.66 median |
| sponza | portrait | 2 | 0.19 | 60.0 | 0.67 | 4.90 | 41.12 | 2.54 | 537 / 465 / 108 / 316 | 13863 / 6954 / 2499 / 3779 / 2743 | r3d.draw +7.3 ms; triangles missed ×1.63 median |
| sponza | portrait | 3 | 0.13 | 59.8 | 0.68 | 4.87 | 41.08 | 2.55 | 537 / 466 / 108 / 314 | 13895 / 6961 / 2521 / 3807 / 2740 | r3d.draw +7.2 ms; triangles missed ×1.64 median |
| sponza | portrait | 4 | 0.72 | 59.6 | 0.68 | 4.79 | 41.65 | 2.55 | 532 / 455 / 101 / 320 | 13586 / 6942 / 2596 / 3516 / 2728 | r3d.draw +7.8 ms; triangles facing away ×1.60 median |
| sponza | portrait | 5 | 36.21 | 59.5 | 0.68 | 5.01 | 41.19 | 2.53 | 540 / 468 / 109 / 317 | 13959 / 6955 / 2504 / 3880 / 2740 | r3d.draw +7.3 ms; triangles missed ×1.67 median |
| sponza | landscape | 1 | 0.06 | 57.6 | 0.67 | 4.90 | 38.95 | 2.54 | 530 / 465 / 114 / 319 | 13848 / 6942 / 2517 / 3767 / 2731 | r3d.draw +7.4 ms; triangles facing away ×1.72 median |
| sponza | landscape | 2 | 36.24 | 57.5 | 0.67 | 4.92 | 39.24 | 2.48 | 530 / 465 / 109 / 321 | 13848 / 6969 / 2529 / 3738 / 2735 | r3d.draw +7.7 ms; triangles facing away ×1.72 median |
| sponza | landscape | 3 | 36.07 | 57.5 | 0.67 | 4.95 | 39.25 | 2.48 | 530 / 465 / 112 / 319 | 13848 / 6971 / 2514 / 3716 / 2758 | r3d.draw +7.7 ms; triangles facing away ×1.74 median |
| sponza | landscape | 4 | 35.96 | 57.4 | 0.67 | 4.88 | 39.00 | 2.54 | 530 / 465 / 109 / 323 | 13848 / 6951 / 2533 / 3752 / 2739 | r3d.draw +7.4 ms; triangles facing away ×1.72 median |
| sponza | landscape | 5 | 0.12 | 57.4 | 0.67 | 4.86 | 39.01 | 2.46 | 530 / 465 / 114 / 316 | 13848 / 6963 / 2494 / 3739 / 2738 | r3d.draw +7.4 ms; triangles facing away ×1.72 median |
| sponza-lite | portrait | 1 | 2.04 | 48.6 | 0.40 | 2.24 | 32.42 | 2.63 | 258 / 227 / 47 / 169 | 7050 / 3530 / 1708 / 1586 / 1393 | r3d.draw +3.9 ms; triangles facing away ×1.49 median |
| sponza-lite | portrait | 2 | 2.52 | 48.5 | 0.41 | 2.14 | 32.72 | 2.63 | 255 / 220 / 45 / 168 | 6826 / 3482 / 1658 / 1390 / 1360 | r3d.draw +4.2 ms; triangles facing away ×1.45 median |
| sponza-lite | portrait | 3 | 2.33 | 48.4 | 0.40 | 2.18 | 32.76 | 2.62 | 257 / 221 / 46 / 170 | 6858 / 3485 / 1682 / 1408 / 1378 | r3d.draw +4.3 ms; triangles facing away ×1.47 median |
| sponza-lite | portrait | 4 | 2.28 | 48.3 | 0.40 | 2.19 | 32.85 | 2.60 | 257 / 221 / 45 / 171 | 6858 / 3497 / 1693 / 1401 / 1397 | r3d.draw +4.4 ms; triangles facing away ×1.49 median |
| sponza-lite | portrait | 5 | 2.42 | 48.2 | 0.43 | 2.19 | 33.05 | 2.65 | 257 / 220 / 43 / 171 | 6826 / 3454 / 1668 / 1410 / 1373 | r3d.draw +4.6 ms; triangles facing away ×1.47 median |
| sponza-lite | landscape | 1 | 0.05 | 46.5 | 0.39 | 2.35 | 30.40 | 2.56 | 259 / 237 / 50 / 176 | 7322 / 3751 / 1669 / 1627 / 1488 | r3d.draw +3.6 ms; triangles facing away ×1.70 median |
| sponza-lite | landscape | 2 | 34.38 | 46.3 | 0.40 | 2.17 | 30.85 | 2.56 | 254 / 221 / 44 / 166 | 6838 / 3491 / 1648 / 1495 / 1355 | r3d.draw +4.0 ms; triangles facing away ×1.55 median |
| sponza-lite | landscape | 3 | 36.20 | 46.2 | 0.39 | 2.38 | 30.57 | 2.54 | 259 / 237 / 47 / 182 | 7322 / 3766 / 1643 / 1601 / 1499 | r3d.draw +3.8 ms; triangles facing away ×1.71 median |
| sponza-lite | landscape | 4 | 34.34 | 46.1 | 0.39 | 2.23 | 30.87 | 2.59 | 254 / 221 / 44 / 165 | 6838 / 3504 / 1636 / 1475 / 1346 | r3d.draw +4.1 ms; triangles facing away ×1.54 median |
| sponza-lite | landscape | 5 | 2.46 | 46.1 | 0.41 | 2.17 | 30.75 | 2.57 | 252 / 219 / 46 / 163 | 6774 / 3436 / 1622 / 1439 / 1335 | r3d.draw +3.9 ms; triangles facing away ×1.53 median |
| sponza-flat | portrait | 1 | 0.05 | 50.6 | 0.65 | 4.59 | 31.92 | 2.62 | 520 / 461 / 105 / 313 | 14083 / 6933 / 2545 / 4083 / 2731 | r3d.draw +6.0 ms; triangles missed ×1.81 median |
| sponza-flat | portrait | 2 | 0.10 | 50.5 | 0.66 | 4.50 | 31.89 | 2.67 | 520 / 459 / 108 / 311 | 14019 / 6965 / 2554 / 3974 / 2744 | r3d.draw +5.9 ms; triangles missed ×1.76 median |
| sponza-flat | portrait | 3 | 0.55 | 50.3 | 0.66 | 4.49 | 32.47 | 2.69 | 520 / 455 / 102 / 313 | 13891 / 6850 / 2575 / 3929 / 2755 | r3d.draw +6.5 ms; triangles missed ×1.74 median |
| sponza-flat | portrait | 4 | 0.50 | 50.2 | 0.67 | 4.52 | 32.38 | 2.60 | 520 / 455 / 96 / 318 | 13891 / 6902 / 2583 / 3904 / 2736 | r3d.draw +6.4 ms; triangles missed ×1.73 median |
| sponza-flat | portrait | 5 | 0.76 | 50.2 | 0.71 | 4.42 | 32.31 | 2.63 | 520 / 449 / 99 / 312 | 13699 / 6861 / 2586 / 3761 / 2718 | r3d.draw +6.3 ms; triangles missed ×1.67 median |
| sponza-flat | landscape | 1 | 0.05 | 49.4 | 0.64 | 4.42 | 30.89 | 2.60 | 505 / 446 / 110 / 307 | 13586 / 6933 / 2560 / 3604 / 2723 | r3d.draw +6.0 ms; triangles facing away ×1.71 median |
| sponza-flat | landscape | 2 | 36.34 | 49.1 | 0.66 | 4.38 | 30.85 | 2.66 | 505 / 446 / 111 / 306 | 13586 / 6928 / 2565 / 3615 / 2717 | r3d.draw +6.0 ms; triangles facing away ×1.71 median |
| sponza-flat | landscape | 3 | 0.44 | 48.9 | 0.63 | 4.41 | 30.94 | 2.60 | 505 / 445 / 107 / 305 | 13554 / 6897 / 2545 / 3579 / 2737 | r3d.draw +6.1 ms; triangles facing away ×1.72 median |
| sponza-flat | landscape | 4 | 36.00 | 48.9 | 0.64 | 4.42 | 31.15 | 2.60 | 505 / 446 / 109 / 309 | 13586 / 6970 / 2597 / 3558 / 2738 | r3d.draw +6.3 ms; triangles facing away ×1.72 median |
| sponza-flat | landscape | 5 | 36.05 | 48.9 | 0.66 | 4.37 | 31.14 | 2.68 | 505 / 446 / 106 / 312 | 13586 / 7000 / 2566 / 3521 / 2745 | r3d.draw +6.3 ms; triangles facing away ×1.73 median |
| sponza-fitted | portrait | 1 | 0.05 | 48.4 | 0.37 | 2.81 | 31.37 | 2.73 | 280 / 260 / 33 / 217 | 7259 / 4532 / 2260 / 1736 / 842 | r3d.draw +4.8 ms; triangles facing away ×2.32 median |
| sponza-fitted | portrait | 2 | 33.89 | 48.3 | 0.39 | 2.47 | 32.01 | 2.73 | 260 / 232 / 36 / 194 | 6498 / 4151 / 2182 / 1332 / 663 | r3d.draw +5.5 ms; triangles facing away ×1.83 median |
| sponza-fitted | portrait | 3 | 2.71 | 48.2 | 0.39 | 2.50 | 32.49 | 2.88 | 268 / 235 / 34 / 199 | 6578 / 4178 / 2187 / 1419 / 650 | r3d.draw +6.0 ms; triangles facing away ×1.79 median |
| sponza-fitted | portrait | 4 | 2.56 | 48.2 | 0.40 | 2.57 | 32.47 | 2.80 | 274 / 238 / 33 / 203 | 6664 / 4232 / 2217 / 1445 / 693 | r3d.draw +5.9 ms; triangles facing away ×1.91 median |
| sponza-fitted | portrait | 5 | 33.84 | 48.2 | 0.38 | 2.48 | 32.17 | 2.65 | 260 / 232 / 34 / 196 | 6498 / 4150 / 2208 / 1322 / 652 | r3d.draw +5.6 ms; triangles facing away ×1.80 median |
| sponza-fitted | landscape | 1 | 0.05 | 45.1 | 0.38 | 2.75 | 28.63 | 2.87 | 279 / 254 / 29 / 222 | 7079 / 4530 / 2231 / 1537 / 863 | r3d.draw +4.0 ms; triangles facing away ×2.37 median |
| sponza-fitted | landscape | 2 | 0.09 | 44.8 | 0.37 | 2.73 | 28.61 | 2.79 | 279 / 254 / 31 / 220 | 7079 / 4550 / 2237 / 1518 / 862 | r3d.draw +4.0 ms; triangles facing away ×2.37 median |
| sponza-fitted | landscape | 3 | 36.06 | 44.7 | 0.37 | 2.75 | 28.98 | 2.76 | 279 / 254 / 30 / 220 | 7079 / 4591 / 2255 / 1486 / 853 | r3d.draw +4.3 ms; triangles facing away ×2.35 median |
| sponza-fitted | landscape | 4 | 35.98 | 44.7 | 0.38 | 2.75 | 28.83 | 2.78 | 279 / 254 / 28 / 223 | 7079 / 4572 / 2244 / 1504 / 854 | r3d.draw +4.2 ms; triangles facing away ×2.35 median |
| sponza-fitted | landscape | 5 | 36.11 | 44.7 | 0.37 | 2.75 | 28.84 | 2.82 | 279 / 254 / 30 / 220 | 7079 / 4563 / 2266 / 1502 / 865 | r3d.draw +4.2 ms; triangles facing away ×2.38 median |
| sponza-fitted-full | portrait | 1 | 0.07 | 61.6 | 0.67 | 5.59 | 41.68 | 2.77 | 550 / 499 / 72 / 410 | 13854 / 8041 / 3563 / 4146 / 1507 | r3d.draw +7.6 ms; triangles facing away ×2.22 median |
| sponza-fitted-full | portrait | 2 | 0.13 | 61.2 | 0.63 | 5.52 | 41.77 | 2.74 | 550 / 497 / 77 / 405 | 13790 / 8059 / 3541 / 4071 / 1500 | r3d.draw +7.7 ms; triangles facing away ×2.21 median |
| sponza-fitted-full | portrait | 3 | 0.19 | 61.2 | 0.64 | 5.56 | 42.13 | 2.74 | 550 / 496 / 75 / 405 | 13762 / 8096 / 3603 / 3997 / 1509 | r3d.draw +8.1 ms; triangles facing away ×2.22 median |
| sponza-fitted-full | portrait | 4 | 35.46 | 60.8 | 0.66 | 5.40 | 41.57 | 2.69 | 538 / 486 / 74 / 397 | 13525 / 7906 / 3572 / 3986 / 1492 | r3d.draw +7.5 ms; triangles facing away ×2.20 median |
| sponza-fitted-full | portrait | 5 | 36.06 | 60.8 | 0.64 | 5.59 | 41.61 | 2.69 | 551 / 502 / 77 / 405 | 13935 / 8042 / 3569 / 4195 / 1537 | r3d.draw +7.5 ms; triangles facing away ×2.27 median |
| sponza-fitted-full | landscape | 1 | 0.12 | 58.1 | 0.62 | 5.49 | 38.66 | 2.82 | 539 / 495 / 77 / 409 | 13716 / 8100 / 3557 / 3912 / 1531 | r3d.draw +7.7 ms; triangles facing away ×2.42 median |
| sponza-fitted-full | landscape | 2 | 0.06 | 58.0 | 0.62 | 5.47 | 38.59 | 2.77 | 539 / 496 / 80 / 406 | 13748 / 8075 / 3553 / 3951 / 1549 | r3d.draw +7.6 ms; triangles facing away ×2.45 median |
| sponza-fitted-full | landscape | 3 | 36.11 | 57.8 | 0.62 | 5.59 | 38.78 | 2.80 | 539 / 496 / 79 / 408 | 13748 / 8117 / 3581 / 3875 / 1586 | r3d.draw +7.8 ms; triangles facing away ×2.51 median |
| sponza-fitted-full | landscape | 4 | 0.18 | 57.7 | 0.62 | 5.50 | 38.77 | 2.79 | 539 / 495 / 76 / 409 | 13716 / 8138 / 3595 / 3854 / 1551 | r3d.draw +7.8 ms; triangles facing away ×2.45 median |
| sponza-fitted-full | landscape | 5 | 36.16 | 57.4 | 0.62 | 5.61 | 38.63 | 2.71 | 539 / 496 / 80 / 407 | 13748 / 8129 / 3543 / 3857 / 1590 | r3d.draw +7.7 ms; triangles facing away ×2.51 median |
| sponza-flat-fitted | portrait | 1 | 0.06 | 51.5 | 0.65 | 5.13 | 31.98 | 2.86 | 542 / 488 / 72 / 393 | 14179 / 8272 / 3533 / 4251 / 1520 | r3d.draw +7.0 ms; triangles facing away ×2.20 median |
| sponza-flat-fitted | portrait | 2 | 0.21 | 50.6 | 0.62 | 5.16 | 31.99 | 2.83 | 542 / 487 / 75 / 387 | 14163 / 8290 / 3553 / 4247 / 1490 | r3d.draw +7.1 ms; triangles facing away ×2.16 median |
| sponza-flat-fitted | portrait | 3 | 35.90 | 50.5 | 0.62 | 5.14 | 31.91 | 2.87 | 542 / 489 / 68 / 396 | 14227 / 8236 / 3557 / 4336 / 1516 | r3d.draw +7.0 ms; triangles facing away ×2.20 median |
| sponza-flat-fitted | portrait | 4 | 0.46 | 50.5 | 0.61 | 5.11 | 31.98 | 2.84 | 532 / 481 / 69 / 391 | 14010 / 8231 / 3520 / 4137 / 1498 | r3d.draw +7.0 ms; triangles facing away ×2.17 median |
| sponza-flat-fitted | portrait | 5 | 36.05 | 50.5 | 0.62 | 5.15 | 31.78 | 2.87 | 542 / 490 / 71 / 396 | 14243 / 8218 / 3521 / 4370 / 1519 | r3d.draw +6.8 ms; triangles facing away ×2.20 median |
| sponza-flat-fitted | landscape | 1 | 0.05 | 49.4 | 0.62 | 5.11 | 30.15 | 2.82 | 540 / 485 / 73 / 400 | 14011 / 8301 / 3539 / 4014 / 1551 | r3d.draw +6.6 ms; triangles facing away ×2.38 median |
| sponza-flat-fitted | landscape | 2 | 0.15 | 49.0 | 0.62 | 5.10 | 30.08 | 2.83 | 540 / 485 / 74 / 398 | 14011 / 8305 / 3535 / 4023 / 1538 | r3d.draw +6.5 ms; triangles facing away ×2.36 median |
| sponza-flat-fitted | landscape | 3 | 36.14 | 48.7 | 0.63 | 5.08 | 30.26 | 2.85 | 540 / 487 / 71 / 402 | 14075 / 8304 / 3556 / 4073 / 1555 | r3d.draw +6.7 ms; triangles facing away ×2.38 median |
| sponza-flat-fitted | landscape | 4 | 35.89 | 48.7 | 0.61 | 5.05 | 30.04 | 2.81 | 540 / 485 / 67 / 404 | 14011 / 8206 / 3562 / 4120 / 1542 | r3d.draw +6.4 ms; triangles facing away ×2.36 median |
| sponza-flat-fitted | landscape | 5 | 0.10 | 48.6 | 0.61 | 5.13 | 30.01 | 2.83 | 540 / 485 / 75 / 397 | 14011 / 8261 / 3520 / 4049 / 1556 | r3d.draw +6.4 ms; triangles facing away ×2.38 median |
| capybara | portrait | 1 | 0.02 | 21.6 | 0.04 | 0.30 | 5.82 | 2.48 | 32 / 32 / 11 / 21 | 994 / 449 / 327 / 21 / 522 | r3d.cull +0.0 ms; triangles missed ×2.47 median |
| capybara | portrait | 2 | 0.59 | 21.3 | 0.04 | 0.30 | 5.95 | 2.49 | 32 / 32 / 8 / 22 | 994 / 456 / 339 / 9 / 527 | r3d.draw +0.1 ms; triangles missed ×1.06 median |
| capybara | portrait | 3 | 0.04 | 21.1 | 0.05 | 0.40 | 5.91 | 2.49 | 32 / 32 / 11 / 21 | 994 / 455 / 326 / 10 / 527 | r3d.transform +0.1 ms; clusters hidden ×1.38 median |
| capybara | portrait | 4 | 0.67 | 21.1 | 0.04 | 0.31 | 5.94 | 2.46 | 32 / 32 / 9 / 23 | 994 / 473 / 334 / 20 / 499 | r3d.draw +0.1 ms; triangles missed ×2.35 median |
| capybara | portrait | 5 | 0.69 | 21.1 | 0.04 | 0.33 | 6.03 | 2.45 | 32 / 32 / 10 / 22 | 994 / 471 / 335 / 17 / 504 | r3d.draw +0.1 ms; triangles missed ×2.00 median |
| capybara | landscape | 1 | 0.50 | 23.0 | 0.04 | 0.34 | 6.32 | 2.76 | 32 / 32 / 10 / 22 | 994 / 453 / 327 / 14 / 525 | r3d.upscale +0.1 ms; clusters tested ×1.00 median |
| capybara | landscape | 2 | 0.30 | 23.0 | 0.05 | 0.40 | 6.33 | 2.70 | 32 / 32 / 10 / 22 | 994 / 453 / 327 / 14 / 525 | r3d.transform +0.1 ms; clusters tested ×1.00 median |
| capybara | landscape | 3 | 0.79 | 23.0 | 0.05 | 0.37 | 6.28 | 2.66 | 32 / 32 / 10 / 22 | 994 / 453 / 327 / 14 / 525 | r3d.transform +0.1 ms; clusters tested ×1.00 median |
| capybara | landscape | 4 | 0.09 | 22.8 | 0.04 | 0.31 | 6.26 | 2.74 | 32 / 32 / 10 / 22 | 994 / 453 / 327 / 14 / 525 | r3d.upscale +0.1 ms; clusters tested ×1.00 median |
| capybara | landscape | 5 | 0.03 | 22.7 | 0.04 | 0.30 | 6.25 | 2.66 | 32 / 32 / 10 / 22 | 994 / 453 / 327 / 14 / 525 | r3d.cull +0.0 ms; clusters tested ×1.00 median |

Sources:

- `frame-profile-1-board.log`
- `frame-profile-2-board.log`
<!-- /generated: frame-timing-profiles -->
