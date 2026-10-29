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
capture. A release image draws only the scene title of the HUD: the fps
and status lines are development builds' alone.

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
| `render water tour` | plays the scripted tour, for repeatable timings: upright, tipped right and left, laid back, flat, toward the viewer, upright, then shaken |
| `render water swing` | plays the swing the GIF below shows: rocked side to side, hard and then calmer, flipped over and straight back twice on the way, leaned back throughout |
| `water.gravity_mm_s2` | the gravity the water runs under; below Earth's, so a 35 mm tank sloshes at a pace a frame can follow |
| `water.substeps` | substeps per frame (1 to 4, default 2), each an equal share of the frame's simulated time, at most 32 ms |
| `water.two_core` | 1 (default) runs part of the particle passes, the surface field, its meshing and the shading on core 1, each cut to balance the two cores' times (`job_run_range()`); 0 keeps them on one core |
| `water.red_black` | 1 (default) solves the pressure in red-black order, each colour split across both cores; 0 in index order on one core |
| `water.samples_pct` | the surface field's density samples per cell, in hundredths (100 to 200, default 150): fewer mesh, build and draw fewer triangles and lose the smallest ripples |
| `water.plane` | 1 draws a plane at the fill's volume instead of flip3, its tilt following gravity's like a damped pendulum (`water.slosh_decihz`, `water.slosh_damping_pct`); much cheaper, but no crests or splash; 0 (default) is flip3 |

Each frame, while the last one goes out to the panel:

| Step | Owner |
|---|---|
| Gravity and shake, in g, box axes | `input/motion/gravity.h` |
| `water.substeps` substeps, none at rest | `physics/fluid/flip3.h` `flip3_step()` |
| The smoothed surface field | `flip3_field()` |
| Its surface, as triangles | `math/geometry/iso_surface.h` `iso_surface_nets()` |
| Shading per vertex, encoded through a table | the scene; `r3d_skin_light_encode()` |
| Clusters and a tree the raster walks | `render/dynamic/r3d_dynamic_mesh.h` |
| Drawn as the water entity | `scene_renderer_set_mesh()` |

![The water scene's swing](../../../../../docs/images/overview/render-lab-water.gif)

`run_water_perf_suite` plays the tour ([`tests/suite_water_perf.c`](../tests/suite_water_perf.c))
under each experiment's tunables and times every stage of the update, `flip3_step()`'s
own stages and r3d's; [`tools/water_tables.py`](water_tables.py) turns its capture into tables.

<!-- generated: water-stages check: python launcher/main/apps/render_lab/tools/water_tables.py --check -->
### Measured stages

The scripted tour at 33 ms of water per frame, one row per phase, from a board capture (2 runs: means are the runs' mean, p95 and worst their worst). Update is the simulation and the surface; draw is r3d drawing the water's dynamic mesh.

| Phase | Frames | Substeps/frame | Triangles | Particles | Frame mean / p95 ms | Update mean / p95 ms | Draw mean / p95 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| rest | 46 | 2.0 | 1265 | 1093 | 77.0 / 78.6 | 50.5 / 52.3 | 13.5 / 14.0 |
| tilt | 136 | 2.0 | 1352 | 1093 | 78.4 / 80.0 | 51.2 / 52.8 | 14.2 / 15.1 |
| flat | 91 | 2.0 | 1565 | 1093 | 81.3 / 87.9 | 52.7 / 55.4 | 15.7 / 19.4 |
| return | 46 | 2.0 | 1315 | 1093 | 77.4 / 78.8 | 51.0 / 52.2 | 13.5 / 13.9 |
| settle | 45 | 2.0 | 1250 | 1093 | 76.8 / 78.4 | 50.2 / 51.7 | 13.7 / 14.0 |
| shake | 60 | 2.0 | 1336 | 1093 | 78.0 / 79.6 | 51.0 / 52.5 | 14.1 / 14.6 |

Where the `base` update and draw go, ms per frame:

| Stage | rest | tilt | flat | return | settle | shake |
|---|---:|---:|---:|---:|---:|---:|
| water.field | 2.56 | 2.45 | 2.46 | 2.46 | 2.46 | 2.46 |
| water.through | 0.50 | 0.49 | 0.52 | 0.49 | 0.47 | 0.48 |
| water.nets | 4.83 | 4.76 | 5.32 | 4.72 | 4.56 | 4.85 |
| water.shade | 1.25 | 1.28 | 1.47 | 1.25 | 1.19 | 1.26 |
| water.build | 3.23 | 3.28 | 3.96 | 3.25 | 3.04 | 3.24 |
| water.step | 38.62 | 38.91 | 38.86 | 38.74 | 38.36 | 38.59 |
| step: p2g | 16.05 | 16.02 | 16.06 | 16.06 | 15.98 | 16.02 |
| step: accel | 0.38 | 0.38 | 0.38 | 0.38 | 0.38 | 0.38 |
| step: pressure | 9.97 | 10.29 | 10.24 | 10.07 | 9.79 | 10.00 |
| step: g2p | 11.02 | 11.01 | 10.98 | 11.03 | 11.02 | 10.99 |
| step: advect | 0.14 | 0.14 | 0.15 | 0.14 | 0.14 | 0.15 |
| step: collide | 0.55 | 0.55 | 0.55 | 0.55 | 0.55 | 0.55 |
| r3d.cull | 0.05 | 0.10 | 0.10 | 0.10 | 0.09 | 0.10 |
| r3d.transform | 0.36 | 0.71 | 0.85 | 0.72 | 0.69 | 0.70 |
| r3d.draw | 6.38 | 13.18 | 14.48 | 12.43 | 12.66 | 13.03 |
| r3d.upscale | 1.50 | 2.94 | 2.93 | 2.92 | 2.93 | 2.95 |

Each experiment against the base, update mean ms per frame:

| Configuration | rest | tilt | flat | return | settle | shake |
|---|---:|---:|---:|---:|---:|---:|
| base | 50.5 | 51.2 | 52.7 | 51.0 | 50.2 | 51.0 |
| one_core | 76.0 | 76.6 | 78.8 | 76.6 | 75.5 | 76.1 |
| sub4 | 88.6 | 89.8 | 91.5 | 89.0 | 88.6 | 89.6 |
| sub1 | 31.0 | 31.6 | 32.8 | 31.1 | 31.0 | 30.2 |
| index_order | 54.0 | 54.1 | 55.9 | 53.9 | 53.5 | 53.5 |
| samples2 | 58.9 | 60.1 | 62.6 | 59.6 | 58.7 | 60.0 |
| plane | 21.1 | 22.4 | 25.1 | 22.0 | 21.1 | 21.5 |

Internal RAM around each configuration, KB free / largest block:

| Configuration | before | running | after |
|---|---:|---:|---:|
| base | 62 / 31 | 58 / 31 | 59 / 31 |
| one_core | 59 / 31 | 58 / 31 | 59 / 31 |
| sub4 | 59 / 31 | 58 / 31 | 59 / 31 |
| sub1 | 59 / 31 | 58 / 31 | 59 / 31 |
| index_order | 59 / 31 | 58 / 31 | 59 / 31 |
| samples2 | 59 / 31 | 58 / 31 | 59 / 31 |
| plane | 59 / 31 | 58 / 31 | 59 / 31 |

Source: `water-stages-board.log`
<!-- /generated: water-stages -->

### Small fill

`render_lab.small_fill_ceiling`, `render_lab.small_fill_gate` and
`render_lab.small_fill_style` apply to the scene camera each frame.
The BOOT menu has a SMALL FILL dropdown with every `SMALL_FILL_` preset;
picking one sets the three tunables. Every menu option is in a release
image too.
The host renderer picks one by its menu label, as
`--small-fill "FAST - DITHER"`.

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

`dynres_quality.sh [--scene-file F --scene KEY] [--args "ARGS"] WORK OUT.csv CAMERA WxH [WxH ...]`
scores each size against the reference along that camera's clip, at
intervals taken from the script and over the clip's full period. Per-path
results belong in `docs/render/data/dynamic-resolution-quality-CAMERA.csv`.
`upscale_quality.sh` runs it per scene under each upscale filter, for
[Dynamic-Resolution.md](../../../../../docs/render/Dynamic-Resolution.md#upscaling-within-a-surface):

```sh
sh launcher/main/apps/render_lab/tools/upscale_quality.sh /tmp/upscale docs/render/data/upscale-filter-quality.csv     docs/images/render/upscale-surface camera launcher/demo/bistro/bistro.scene.toml:bistro-fitted-full     launcher/demo/sponza/sponza.scene.toml:sponza-fitted-full launcher/demo/sponza/sponza.scene.toml:sponza-fitted     launcher/demo/sponza/sponza.scene.toml:sponza-flat-fitted launcher/demo/sponza/sponza.scene.toml:sponza     launcher/demo/sponza/sponza.scene.toml:sponza-flat launcher/demo/sponza/sponza.scene.toml:sponza-lite
```

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
| `sh launcher/main/apps/render_lab/tools/floor_shots.sh [--flash] --out <dir>/shots.txt <scene>@<ms>...` | screenshots each pose at the chosen size and at the dynamic-resolution floor, into `<dir>` |
| `autana render pose <ms>` | holds a Sponza or Bistro scene's camera at that time along its path; `autana render pose play` lets it move again |
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
| `render-lab-capybara.gif` | every clip the capybara scene lists, in its order and for an equal time, each blended into the next and, more slowly, back to the first, as the camera turns once around it |
| `render-lab-water.gif` | the water scene's swing (`render water swing`): rocked side to side, hard and then calmer, flipped over and straight back twice on the way, leaned back so the surface shows, in the `FAST - DITHER` small fill; landscape, as the other clips |

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

`run_render_lab_frame_profile_suite` plays each Sponza scene's camera clip,
Bistro fitted-full's camera clip and the capybara gallop for one clip period in each perf quarter. It times
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

<!-- generated: frame-timing-profiles check: python launcher/main/apps/render_lab/tools/frame_profile_tables.py --worst-board 1 --scenes sponza-fitted-full,sponza-fitted,sponza,sponza-lite,sponza-flat,sponza-flat-fitted,capybara,bistro-fitted-full --check -->
### Measured frame rate

Cells list board 1 / board 2.
Playback includes update, render, compose and present.

| Scene | Quarter | Mean fps | p50 ms | p95 ms | Worst ms |
|---|---|---:|---:|---:|---:|
| sponza-fitted-full | portrait | 20.8 / 20.8 | 48.7 / 48.7 | 60.1 / 60.1 | 61.3 / 61.3 |
| sponza-fitted-full | landscape | 22.0 / 22.0 | 46.1 / 46.1 | 55.6 / 55.5 | 58.0 / 58.1 |
| sponza-fitted | portrait | 25.0 / 25.0 | 40.5 / 40.5 | 47.3 / 47.3 | 48.3 / 48.3 |
| sponza-fitted | landscape | 26.3 / 26.2 | 38.5 / 38.5 | 44.1 / 44.0 | 45.0 / 45.1 |
| sponza | portrait | 20.6 / 20.5 | 49.2 / 49.3 | 58.9 / 59.0 | 60.6 / 60.7 |
| sponza | landscape | 21.3 / 21.3 | 47.1 / 47.2 | 55.9 / 56.0 | 57.7 / 57.7 |
| sponza-lite | portrait | 24.0 / 24.0 | 42.3 / 42.3 | 47.5 / 47.5 | 48.5 / 48.4 |
| sponza-lite | landscape | 24.8 / 24.8 | 40.8 / 40.8 | 45.7 / 45.7 | 46.7 / 46.4 |
| sponza-flat | portrait | 24.6 / 24.6 | 40.7 / 40.8 | 49.4 / 49.5 | 50.4 / 50.6 |
| sponza-flat | landscape | 25.0 / 25.0 | 40.2 / 40.3 | 48.0 / 48.1 | 48.9 / 49.2 |
| sponza-flat-fitted | portrait | 25.3 / 25.2 | 40.2 / 40.3 | 49.6 / 49.7 | 51.4 / 51.3 |
| sponza-flat-fitted | landscape | 26.2 / 26.2 | 38.7 / 38.8 | 47.3 / 47.3 | 48.8 / 48.8 |
| capybara | portrait | 48.2 / 48.1 | 20.8 / 20.7 | 21.2 / 21.3 | 21.6 / 21.6 |
| capybara | landscape | 46.8 / 46.9 | 21.4 / 21.3 | 21.8 / 21.7 | 21.9 / 21.8 |
| bistro-fitted-full | portrait | 26.3 / 26.3 | 38.0 / 38.0 | 58.3 / 58.2 | 63.5 / 63.2 |
| bistro-fitted-full | landscape | 25.0 / 25.0 | 40.3 / 40.3 | 57.1 / 57.1 | 59.6 / 59.7 |

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
![bistro-fitted-full: frames along the path and the cull's counts](images/worst-frames/bistro-fitted-full.png)

#### Worst frames, ranked

Played frames ranked by wall time. Stage ms and counts come from replaying each pose paused;
Why compares them with the median of the 64 poses spaced evenly along the path (1024 of 1024 read).

| Scene | Quarter | Rank | Path s | Frame ms | r3d.cull ms | r3d.transform ms | r3d.draw ms | r3d.upscale ms | Clusters tested / kept / hidden / drawn | Triangles submitted / set up / cover / miss / away | Why |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| sponza | portrait | 1 | 0.07 | 60.6 | 0.68 | 4.78 | 40.97 | 2.73 | 537 / 467 / 105 / 318 | 13927 / 6945 / 2519 / 3856 / 2739 | r3d.draw +7.2 ms; triangles missed ×1.66 median |
| sponza | portrait | 2 | 0.19 | 60.0 | 0.68 | 4.78 | 41.29 | 2.77 | 537 / 465 / 108 / 316 | 13863 / 6954 / 2499 / 3779 / 2743 | r3d.draw +7.5 ms; triangles missed ×1.63 median |
| sponza | portrait | 3 | 0.13 | 59.9 | 0.67 | 4.78 | 41.07 | 2.78 | 537 / 466 / 108 / 314 | 13895 / 6961 / 2521 / 3807 / 2740 | r3d.draw +7.3 ms; triangles missed ×1.64 median |
| sponza | portrait | 4 | 36.23 | 59.7 | 0.68 | 4.86 | 41.32 | 2.76 | 540 / 468 / 101 / 322 | 13959 / 6962 / 2507 / 3865 / 2748 | r3d.draw +7.5 ms; triangles missed ×1.67 median |
| sponza | portrait | 5 | 0.72 | 59.7 | 0.67 | 4.77 | 41.80 | 2.82 | 532 / 455 / 101 / 320 | 13586 / 6940 / 2601 / 3518 / 2728 | r3d.draw +8.0 ms; triangles facing away ×1.60 median |
| sponza | landscape | 1 | 0.06 | 57.7 | 0.68 | 4.75 | 39.00 | 2.72 | 530 / 465 / 113 / 319 | 13848 / 6942 / 2523 / 3764 / 2734 | r3d.draw +7.5 ms; triangles facing away ×1.72 median |
| sponza | landscape | 2 | 36.11 | 57.5 | 0.68 | 4.80 | 39.05 | 2.73 | 530 / 465 / 113 / 318 | 13848 / 6947 / 2500 / 3748 / 2750 | r3d.draw +7.5 ms; triangles facing away ×1.73 median |
| sponza | landscape | 3 | 36.05 | 57.5 | 0.70 | 4.84 | 39.24 | 2.72 | 530 / 465 / 111 / 319 | 13848 / 6999 / 2525 / 3701 / 2745 | r3d.draw +7.7 ms; triangles facing away ×1.73 median |
| sponza | landscape | 4 | 0.12 | 57.5 | 0.67 | 4.75 | 38.98 | 2.80 | 530 / 465 / 114 / 316 | 13848 / 6963 / 2494 / 3739 / 2738 | r3d.draw +7.5 ms; triangles facing away ×1.72 median |
| sponza | landscape | 5 | 36.28 | 57.4 | 0.68 | 4.80 | 39.08 | 2.82 | 530 / 465 / 114 / 318 | 13848 / 6978 / 2512 / 3723 / 2741 | r3d.draw +7.6 ms; triangles facing away ×1.73 median |
| sponza-lite | portrait | 1 | 2.33 | 48.5 | 0.40 | 2.20 | 32.68 | 2.66 | 257 / 221 / 47 / 169 | 6858 / 3483 / 1661 / 1409 / 1379 | r3d.draw +4.2 ms; triangles facing away ×1.47 median |
| sponza-lite | portrait | 2 | 2.43 | 48.4 | 0.41 | 2.18 | 32.99 | 2.65 | 257 / 220 / 43 / 171 | 6826 / 3443 / 1658 / 1427 / 1367 | r3d.draw +4.5 ms; triangles facing away ×1.46 median |
| sponza-lite | portrait | 3 | 2.28 | 48.2 | 0.40 | 2.19 | 32.89 | 2.65 | 257 / 221 / 45 / 171 | 6858 / 3487 / 1686 / 1400 / 1407 | r3d.draw +4.4 ms; triangles facing away ×1.50 median |
| sponza-lite | portrait | 4 | 2.09 | 48.2 | 0.40 | 2.25 | 32.62 | 2.71 | 258 / 225 / 46 / 169 | 6986 / 3556 / 1709 / 1475 / 1412 | r3d.draw +4.1 ms; triangles facing away ×1.51 median |
| sponza-lite | portrait | 5 | 0.05 | 48.2 | 0.40 | 2.43 | 31.97 | 2.66 | 264 / 243 / 52 / 174 | 7508 / 3722 / 1667 / 1844 / 1496 | r3d.draw +3.5 ms; triangles facing away ×1.60 median |
| sponza-lite | landscape | 1 | 0.05 | 46.7 | 0.39 | 2.38 | 30.56 | 2.66 | 259 / 237 / 50 / 176 | 7322 / 3751 / 1669 / 1627 / 1488 | r3d.draw +3.8 ms; triangles facing away ×1.70 median |
| sponza-lite | landscape | 2 | 36.04 | 46.5 | 0.39 | 2.34 | 30.75 | 2.66 | 259 / 237 / 48 / 180 | 7322 / 3743 / 1667 / 1632 / 1491 | r3d.draw +3.9 ms; triangles facing away ×1.70 median |
| sponza-lite | landscape | 3 | 34.35 | 46.5 | 0.39 | 2.20 | 31.05 | 2.67 | 254 / 221 / 42 / 167 | 6838 / 3503 / 1648 / 1481 / 1345 | r3d.draw +4.2 ms; triangles facing away ×1.54 median |
| sponza-lite | landscape | 4 | 34.30 | 46.5 | 0.40 | 2.19 | 30.88 | 2.67 | 254 / 221 / 45 / 165 | 6838 / 3489 / 1645 / 1490 / 1338 | r3d.draw +4.1 ms; triangles facing away ×1.53 median |
| sponza-lite | landscape | 5 | 35.90 | 46.4 | 0.39 | 2.33 | 30.73 | 2.67 | 259 / 237 / 48 / 180 | 7322 / 3761 / 1677 / 1619 / 1486 | r3d.draw +3.9 ms; triangles facing away ×1.70 median |
| sponza-flat | portrait | 1 | 0.05 | 50.4 | 0.66 | 4.49 | 31.89 | 2.47 | 520 / 461 / 105 / 313 | 14083 / 6933 / 2545 / 4083 / 2731 | r3d.draw +6.0 ms; triangles missed ×1.81 median |
| sponza-flat | portrait | 2 | 0.55 | 50.1 | 0.67 | 4.44 | 32.38 | 2.53 | 520 / 455 / 101 / 314 | 13891 / 6861 / 2587 / 3920 / 2753 | r3d.draw +6.5 ms; triangles missed ×1.74 median |
| sponza-flat | portrait | 3 | 0.15 | 50.1 | 0.66 | 4.48 | 31.97 | 2.44 | 520 / 459 / 107 / 310 | 14019 / 6956 / 2556 / 3979 / 2746 | r3d.draw +6.1 ms; triangles missed ×1.77 median |
| sponza-flat | portrait | 4 | 0.65 | 50.0 | 0.66 | 4.46 | 32.21 | 2.46 | 520 / 453 / 99 / 313 | 13827 / 6844 / 2555 / 3883 / 2733 | r3d.draw +6.3 ms; triangles missed ×1.72 median |
| sponza-flat | portrait | 5 | 0.10 | 50.0 | 0.66 | 4.52 | 31.85 | 2.47 | 520 / 459 / 109 / 310 | 14019 / 6962 / 2548 / 3976 / 2745 | r3d.draw +5.9 ms; triangles missed ×1.76 median |
| sponza-flat | landscape | 1 | 0.05 | 48.9 | 0.63 | 4.33 | 30.96 | 2.42 | 505 / 446 / 110 / 307 | 13586 / 6935 / 2561 / 3603 / 2722 | r3d.draw +6.2 ms; triangles facing away ×1.71 median |
| sponza-flat | landscape | 2 | 0.39 | 48.8 | 0.63 | 4.33 | 30.87 | 2.44 | 505 / 445 / 107 / 306 | 13554 / 6892 / 2540 / 3586 / 2741 | r3d.draw +6.1 ms; triangles facing away ×1.73 median |
| sponza-flat | landscape | 3 | 0.73 | 48.7 | 0.64 | 4.35 | 30.85 | 2.54 | 505 / 443 / 103 / 303 | 13490 / 6839 / 2518 / 3609 / 2685 | r3d.draw +6.1 ms; triangles facing away ×1.69 median |
| sponza-flat | landscape | 4 | 0.10 | 48.7 | 0.63 | 4.34 | 30.80 | 2.42 | 505 / 446 / 108 / 309 | 13586 / 6958 / 2530 / 3569 / 2733 | r3d.draw +6.1 ms; triangles facing away ×1.72 median |
| sponza-flat | landscape | 5 | 35.98 | 48.7 | 0.63 | 4.35 | 30.94 | 2.42 | 505 / 446 / 105 / 312 | 13586 / 6958 / 2578 / 3560 / 2748 | r3d.draw +6.2 ms; triangles facing away ×1.73 median |
| sponza-fitted | portrait | 1 | 0.05 | 48.3 | 0.38 | 2.76 | 31.38 | 2.63 | 280 / 260 / 33 / 217 | 7259 / 4532 / 2260 / 1736 / 842 | r3d.draw +4.8 ms; triangles facing away ×2.32 median |
| sponza-fitted | portrait | 2 | 2.70 | 48.2 | 0.40 | 2.43 | 32.40 | 2.66 | 268 / 235 / 33 / 200 | 6578 / 4172 / 2197 / 1426 / 652 | r3d.draw +5.8 ms; triangles facing away ×1.80 median |
| sponza-fitted | portrait | 3 | 2.56 | 48.1 | 0.44 | 2.46 | 32.47 | 2.67 | 274 / 238 / 34 / 202 | 6664 / 4241 / 2211 / 1431 / 701 | r3d.draw +5.9 ms; triangles facing away ×1.93 median |
| sponza-fitted | portrait | 4 | 2.51 | 48.1 | 0.40 | 2.52 | 32.49 | 2.66 | 274 / 238 / 33 / 202 | 6664 / 4290 / 2248 / 1400 / 691 | r3d.draw +5.9 ms; triangles facing away ×1.90 median |
| sponza-fitted | portrait | 5 | 33.88 | 47.9 | 0.39 | 2.41 | 31.93 | 2.67 | 260 / 232 / 35 / 195 | 6498 / 4143 / 2163 / 1342 / 654 | r3d.draw +5.3 ms; triangles facing away ×1.80 median |
| sponza-fitted | landscape | 1 | 0.05 | 45.0 | 0.37 | 2.71 | 28.58 | 2.64 | 279 / 254 / 29 / 222 | 7079 / 4530 / 2231 / 1537 / 863 | r3d.draw +4.0 ms; triangles facing away ×2.37 median |
| sponza-fitted | landscape | 2 | 35.88 | 44.8 | 0.37 | 2.71 | 28.66 | 2.63 | 279 / 254 / 31 / 220 | 7079 / 4568 / 2262 / 1513 / 849 | r3d.draw +4.1 ms; triangles facing away ×2.34 median |
| sponza-fitted | landscape | 3 | 0.09 | 44.7 | 0.37 | 2.68 | 28.73 | 2.63 | 279 / 254 / 31 / 220 | 7079 / 4550 / 2237 / 1518 / 862 | r3d.draw +4.2 ms; triangles facing away ×2.37 median |
| sponza-fitted | landscape | 4 | 35.97 | 44.7 | 0.37 | 2.69 | 28.83 | 2.62 | 279 / 254 / 28 / 223 | 7079 / 4568 / 2248 / 1508 / 854 | r3d.draw +4.3 ms; triangles facing away ×2.35 median |
| sponza-fitted | landscape | 5 | 0.18 | 44.7 | 0.37 | 2.69 | 28.75 | 2.62 | 279 / 254 / 29 / 221 | 7079 / 4576 / 2269 / 1506 / 848 | r3d.draw +4.2 ms; triangles facing away ×2.33 median |
| sponza-fitted-full | portrait | 1 | 0.07 | 61.3 | 0.66 | 5.48 | 41.73 | 2.63 | 550 / 499 / 72 / 410 | 13854 / 8041 / 3563 / 4146 / 1507 | r3d.draw +7.8 ms; triangles facing away ×2.22 median |
| sponza-fitted-full | portrait | 2 | 0.13 | 61.0 | 0.64 | 5.38 | 41.73 | 2.61 | 550 / 497 / 77 / 405 | 13790 / 8057 / 3536 / 4073 / 1500 | r3d.draw +7.8 ms; triangles facing away ×2.21 median |
| sponza-fitted-full | portrait | 3 | 35.49 | 61.0 | 0.62 | 5.33 | 41.61 | 2.64 | 538 / 488 / 77 / 397 | 13584 / 7939 / 3555 / 4037 / 1467 | r3d.draw +7.6 ms; triangles facing away ×2.16 median |
| sponza-fitted-full | portrait | 4 | 0.19 | 60.9 | 0.65 | 5.47 | 42.02 | 2.58 | 550 / 496 / 75 / 405 | 13762 / 8104 / 3599 / 3990 / 1508 | r3d.draw +8.1 ms; triangles facing away ×2.22 median |
| sponza-fitted-full | portrait | 5 | 36.10 | 60.7 | 0.64 | 5.45 | 41.68 | 2.66 | 551 / 503 / 76 / 408 | 13962 / 8072 / 3568 / 4213 / 1516 | r3d.draw +7.7 ms; triangles facing away ×2.23 median |
| sponza-fitted-full | landscape | 1 | 0.06 | 58.0 | 0.62 | 5.40 | 38.58 | 2.64 | 539 / 496 / 80 / 406 | 13748 / 8075 / 3553 / 3951 / 1549 | r3d.draw +7.6 ms; triangles facing away ×2.45 median |
| sponza-fitted-full | landscape | 2 | 0.12 | 57.8 | 0.62 | 5.36 | 38.57 | 2.67 | 539 / 495 / 77 / 409 | 13716 / 8100 / 3557 / 3912 / 1531 | r3d.draw +7.6 ms; triangles facing away ×2.42 median |
| sponza-fitted-full | landscape | 3 | 0.18 | 57.8 | 0.63 | 5.42 | 38.72 | 2.68 | 539 / 495 / 76 / 409 | 13716 / 8133 / 3591 / 3854 / 1556 | r3d.draw +7.7 ms; triangles facing away ×2.46 median |
| sponza-fitted-full | landscape | 4 | 36.11 | 57.7 | 0.62 | 5.35 | 38.81 | 2.69 | 539 / 496 / 77 / 410 | 13748 / 8129 / 3584 / 3860 / 1587 | r3d.draw +7.8 ms; triangles facing away ×2.51 median |
| sponza-fitted-full | landscape | 5 | 36.17 | 57.3 | 0.63 | 5.36 | 38.62 | 2.63 | 539 / 496 / 81 / 406 | 13748 / 8115 / 3550 / 3864 / 1597 | r3d.draw +7.6 ms; triangles facing away ×2.52 median |
| sponza-flat-fitted | portrait | 1 | 0.06 | 51.4 | 0.61 | 5.11 | 31.88 | 2.55 | 542 / 488 / 72 / 393 | 14179 / 8272 / 3533 / 4251 / 1520 | r3d.draw +6.9 ms; triangles facing away ×2.20 median |
| sponza-flat-fitted | portrait | 2 | 0.51 | 50.4 | 0.61 | 4.96 | 31.95 | 2.58 | 532 / 480 / 69 / 388 | 13989 / 8212 / 3494 / 4139 / 1491 | r3d.draw +7.0 ms; triangles facing away ×2.16 median |
| sponza-flat-fitted | portrait | 3 | 0.61 | 50.3 | 0.60 | 4.98 | 31.99 | 2.59 | 531 / 477 / 71 / 387 | 13893 / 8217 / 3497 / 4080 / 1479 | r3d.draw +7.0 ms; triangles facing away ×2.15 median |
| sponza-flat-fitted | portrait | 4 | 36.22 | 50.2 | 0.64 | 5.09 | 31.88 | 2.57 | 542 / 490 / 72 / 393 | 14243 / 8256 / 3542 / 4323 / 1531 | r3d.draw +6.9 ms; triangles facing away ×2.22 median |
| sponza-flat-fitted | portrait | 5 | 2.44 | 50.2 | 0.61 | 4.57 | 32.29 | 2.60 | 498 / 447 / 62 / 376 | 12991 / 7832 / 3528 / 3590 / 1326 | r3d.draw +7.3 ms; triangles facing away ×1.92 median |
| sponza-flat-fitted | landscape | 1 | 0.05 | 48.8 | 0.61 | 4.97 | 30.14 | 2.54 | 540 / 485 / 73 / 400 | 14011 / 8301 / 3539 / 4014 / 1551 | r3d.draw +6.5 ms; triangles facing away ×2.38 median |
| sponza-flat-fitted | landscape | 2 | 36.05 | 48.8 | 0.61 | 4.97 | 30.07 | 2.52 | 540 / 485 / 71 / 402 | 14011 / 8249 / 3549 / 4079 / 1542 | r3d.draw +6.5 ms; triangles facing away ×2.36 median |
| sponza-flat-fitted | landscape | 3 | 0.10 | 48.7 | 0.62 | 4.98 | 30.04 | 2.60 | 540 / 485 / 75 / 397 | 14011 / 8260 / 3520 / 4052 / 1554 | r3d.draw +6.4 ms; triangles facing away ×2.38 median |
| sponza-flat-fitted | landscape | 4 | 36.24 | 48.5 | 0.62 | 4.99 | 30.08 | 2.51 | 540 / 487 / 71 / 402 | 14075 / 8265 / 3532 / 4114 / 1553 | r3d.draw +6.5 ms; triangles facing away ×2.38 median |
| sponza-flat-fitted | landscape | 5 | 0.25 | 48.5 | 0.61 | 4.99 | 30.12 | 2.54 | 539 / 484 / 73 / 397 | 13979 / 8273 / 3540 / 4038 / 1523 | r3d.draw +6.5 ms; triangles facing away ×2.33 median |
| capybara | portrait | 1 | 0.02 | 21.6 | 0.04 | 0.34 | 5.90 | 2.51 | 32 / 32 / 11 / 21 | 994 / 449 / 327 / 21 / 522 | r3d.transform +0.0 ms; triangles missed ×2.47 median |
| capybara | portrait | 2 | 0.04 | 21.2 | 0.04 | 0.33 | 5.90 | 2.56 | 32 / 32 / 11 / 21 | 994 / 455 / 326 / 10 / 527 | r3d.transform +0.0 ms; clusters hidden ×1.38 median |
| capybara | portrait | 3 | 0.75 | 21.1 | 0.04 | 0.29 | 5.87 | 2.67 | 32 / 32 / 10 / 22 | 994 / 459 / 329 / 18 / 515 | r3d.upscale +0.1 ms; triangles missed ×2.12 median |
| capybara | portrait | 4 | 0.15 | 21.0 | 0.04 | 0.35 | 5.93 | 2.53 | 32 / 32 / 10 / 21 | 994 / 439 / 321 / 11 / 542 | r3d.transform +0.0 ms; triangles missed ×1.29 median |
| capybara | portrait | 5 | 0.25 | 20.9 | 0.04 | 0.30 | 5.83 | 2.56 | 32 / 32 / 7 / 22 | 994 / 428 / 341 / 10 / 554 | r3d.cull +0.0 ms; triangles missed ×1.18 median |
| capybara | landscape | 1 | 0.02 | 21.9 | 0.04 | 0.31 | 6.28 | 2.78 | 32 / 32 / 11 / 21 | 994 / 454 / 325 / 15 / 523 | r3d.upscale +0.1 ms; triangles missed ×1.67 median |
| capybara | landscape | 2 | 0.43 | 21.8 | 0.04 | 0.33 | 6.36 | 2.72 | 32 / 32 / 8 / 22 | 994 / 441 / 336 / 7 / 544 | r3d.upscale +0.0 ms; triangles facing away ×1.01 median |
| capybara | landscape | 3 | 0.54 | 21.6 | 0.04 | 0.30 | 6.47 | 2.69 | 32 / 32 / 8 / 22 | 994 / 452 / 337 / 5 / 535 | r3d.draw +0.1 ms; triangles set up ×1.01 median |
| capybara | landscape | 4 | 0.73 | 21.6 | 0.04 | 0.30 | 6.36 | 2.66 | 32 / 32 / 11 / 21 | 994 / 460 / 328 / 17 / 515 | r3d.draw +0.0 ms; triangles missed ×1.89 median |
| capybara | landscape | 5 | 0.45 | 21.5 | 0.04 | 0.32 | 6.49 | 2.70 | 32 / 32 / 8 / 22 | 994 / 444 / 333 / 4 / 544 | r3d.draw +0.2 ms; triangles facing away ×1.01 median |
| bistro-fitted-full | portrait | 1 | 59.53 | 63.5 | 0.72 | 7.24 | 42.25 | 2.49 | 656 / 621 / 251 / 327 | 12728 / 6618 / 2578 / 3238 / 2804 | r3d.draw +17.5 ms; clusters hidden ×2.00 median |
| bistro-fitted-full | portrait | 2 | 58.21 | 63.3 | 0.72 | 7.31 | 42.35 | 2.47 | 656 / 625 / 264 / 315 | 12784 / 6678 / 2542 / 3294 / 2789 | r3d.draw +17.6 ms; clusters hidden ×2.10 median |
| bistro-fitted-full | portrait | 3 | 59.22 | 63.3 | 0.72 | 7.22 | 42.49 | 2.49 | 656 / 622 / 256 / 325 | 12749 / 6602 / 2585 / 3289 / 2798 | r3d.draw +17.8 ms; clusters hidden ×2.04 median |
| bistro-fitted-full | portrait | 4 | 58.34 | 63.2 | 0.74 | 7.33 | 42.40 | 2.46 | 656 / 627 / 258 / 315 | 12823 / 6657 / 2540 / 3352 / 2785 | r3d.draw +17.7 ms; clusters hidden ×2.06 median |
| bistro-fitted-full | portrait | 5 | 58.59 | 63.1 | 0.73 | 7.29 | 42.51 | 2.51 | 656 / 627 / 260 / 316 | 12823 / 6640 / 2478 / 3386 / 2761 | r3d.draw +17.8 ms; clusters hidden ×2.07 median |
| bistro-fitted-full | landscape | 1 | 60.59 | 59.6 | 0.77 | 7.64 | 38.27 | 2.39 | 737 / 654 / 283 / 318 | 13408 / 6706 / 2433 / 3757 / 2842 | r3d.draw +12.1 ms; clusters hidden ×2.17 median |
| bistro-fitted-full | landscape | 2 | 60.12 | 59.5 | 0.77 | 7.69 | 38.12 | 2.39 | 735 / 651 / 281 / 313 | 13322 / 6641 / 2417 / 3793 / 2785 | r3d.draw +11.9 ms; clusters hidden ×2.15 median |
| bistro-fitted-full | landscape | 3 | 61.30 | 59.4 | 0.80 | 7.73 | 38.04 | 2.38 | 728 / 661 / 284 / 321 | 13546 / 6707 / 2412 / 3820 / 2881 | r3d.draw +11.9 ms; clusters hidden ×2.18 median |
| bistro-fitted-full | landscape | 4 | 61.72 | 59.4 | 0.77 | 7.73 | 37.95 | 2.37 | 732 / 663 / 282 / 320 | 13588 / 6639 / 2395 / 3934 / 2878 | r3d.draw +11.8 ms; clusters hidden ×2.16 median |
| bistro-fitted-full | landscape | 5 | 60.00 | 59.4 | 0.78 | 7.57 | 38.32 | 2.37 | 735 / 651 / 278 / 317 | 13322 / 6609 / 2422 / 3854 / 2760 | r3d.draw +12.1 ms; clusters hidden ×2.13 median |

Sources:

- `frame-profile-1-board.log`
- `frame-profile-2-board.log`
<!-- /generated: frame-timing-profiles -->
