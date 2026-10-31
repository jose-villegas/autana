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

Below it, UPSCALE picks how a draw half the panel's width or less fills the panel:
SHARP repeats pixels (the default), SMOOTH blends within a surface and keeps
depth edges sharp
([Dynamic-Resolution.md](../../../../../docs/render/frame/Dynamic-Resolution.md#upscaling-within-a-surface)).
It sets `render_lab.upscale`.

The generic scene viewer accepts `--small-fill CEILING,GATE,STYLE`; its defaults
come from `SMALL_FILL_SMOOTH`. Styles use the `small_fill_style_t` values.
The [engine feature](../../../../../docs/render/frame/Coverage.md#small-fill)
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
engine page shows the [presentation GIFs](../../../../../docs/render/frame/Dynamic-Resolution.md#camera-presentation).

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
`--dump DIR` writes each frame's raster colour and depth, before the
upscale, to `DIR/NNNN.raw` for the frame generation experiment
([Frame-Generation.md](../../../../../docs/render/frame/Frame-Generation.md)).

`dynres_quality.sh [--scene-file F --scene KEY] [--args "ARGS"] WORK OUT.csv CAMERA WxH [WxH ...]`
scores each size against the reference along that camera's clip, at
intervals taken from the script and over the clip's full period. Per-path
results belong in `docs/render/data/dynamic-resolution-quality-CAMERA.csv`.
`upscale_quality.sh` runs it per scene under each upscale filter, for
[Dynamic-Resolution.md](../../../../../docs/render/frame/Dynamic-Resolution.md#upscaling-within-a-surface):

```sh
sh launcher/main/apps/render_lab/tools/upscale_quality.sh /tmp/upscale docs/render/data/upscale-filter-quality.csv     docs/render/data/upscale-frames camera launcher/demo/bistro/bistro.scene.toml:bistro-fitted-full     launcher/demo/sponza/sponza.scene.toml:sponza-fitted-full launcher/demo/sponza/sponza.scene.toml:sponza-fitted     launcher/demo/sponza/sponza.scene.toml:sponza-flat-fitted launcher/demo/sponza/sponza.scene.toml:sponza     launcher/demo/sponza/sponza.scene.toml:sponza-flat launcher/demo/sponza/sponza.scene.toml:sponza-lite
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
| `autana render view <name>` | holds the Capybara scene's camera still on a named view: `home`, `front`, `side`, `back`, or `close` (home's angle at the zoom-in limit) |
| `autana render scenes` | every scene's key and name, and which one is showing |
| `autana render scene <key>` | switches to the scene with exactly that key, such as `sponza`, `sponza-lite` or `sponza-flat-fitted` |
| `sh launcher/main/apps/render_lab/tools/floor_shots.sh [--flash] --out <dir>/shots.txt <scene>@<ms>...` | screenshots each pose at the chosen size and at the dynamic-resolution floor, into `<dir>` |
| `autana render pose <ms>` | holds a Sponza or Bistro scene's camera at that time along its path; `autana render pose play` lets it move again |
| `autana tune render_lab.scale <n>` | the fixed render scale in hundredths of the panel: 200 is half size |
| `autana tune render_lab.budget <ms>` | dynamic resolution on the scene showing; 0 turns it off, as does the menu's DYNAMIC RES |
| `autana tune render_lab.framegen 1` | every other frame is generated from the one before instead of drawn; 0 draws them all, as does the menu's FRAME GEN |
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
[skinned-mesh lighting](../../../../../docs/render/animation/Skinned-Lighting.md)
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
[Baked-Mesh](../../../../../docs/render/offline/Baked-Mesh.md#meshlets).

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

After each playback the suite finds its frame peaks: the path is a loop, so
a peak across its end is one. A peak counts when it stands
`FRAME_PROFILE_PEAK_PERCENT` of its height above the higher of
its two saddles, and its slowest frame stands for it. The suite then replays
poses paused: `FRAME_PROFILE_PATH_POSES` spread evenly along the path, then
every frame peak's, highest first.
Each replay draws one frame through the `cull` view for its counts and
`FRAME_PROFILE_REPLAY_DRAWS` shaded frames for their frame-cost report.
A paused frame does not skin again, so capybara's replayed split has no
skinning in it. `report_performance.py` ranks the frame peaks with those
splits and counts. Its "why" names the stage that most exceeds the path's
median pose and the count furthest above its median.

The [perf tools](../../../../tools/perf/README.md#real-time-playback)
define playback timing and the `frame_profile:` line format.

Each suite owns a table of mean goals and p95 ceilings per row and quarter.
Zero means unpegged: it logs without enforcing a budget. Board ceilings
are reported without enforcement under QEMU.

<!-- generated: frame-timing-profiles check: python launcher/main/apps/render_lab/tools/frame_profile_tables.py --peak-board 1 --scenes sponza-fitted-full,sponza-fitted,sponza,sponza-lite,sponza-flat,sponza-flat-fitted,capybara,capybara-front,capybara-side,capybara-back,capybara-close,capybara-blend,bistro-fitted-full --check -->
### Measured frame rate

Cells list board 1 / board 2.
Playback includes update, render, compose and present.

| Scene | Quarter | Mean fps | p50 ms | p95 ms | Worst ms |
|---|---|---:|---:|---:|---:|
| sponza-fitted-full | portrait | 18.3 / 18.2 | 55.7 / 55.8 | 67.2 / 67.2 | 68.9 / 69.0 |
| sponza-fitted-full | landscape | 19.0 / 19.0 | 53.7 / 53.7 | 63.9 / 63.9 | 65.4 / 65.5 |
| sponza-fitted | portrait | 21.9 / 21.9 | 46.3 / 46.3 | 52.9 / 53.0 | 53.9 / 54.4 |
| sponza-fitted | landscape | 22.8 / 22.8 | 44.3 / 44.5 | 50.1 / 50.2 | 51.1 / 51.2 |
| sponza | portrait | 18.0 / 18.0 | 56.1 / 56.2 | 66.4 / 66.4 | 67.8 / 67.9 |
| sponza | landscape | 18.6 / 18.6 | 54.2 / 54.3 | 63.8 / 64.0 | 65.0 / 65.1 |
| sponza-lite | portrait | 21.1 / 21.1 | 47.9 / 48.0 | 52.8 / 52.9 | 53.9 / 54.0 |
| sponza-lite | landscape | 21.6 / 21.6 | 46.8 / 46.9 | 51.4 / 51.5 | 52.3 / 52.4 |
| sponza-flat | portrait | 21.7 / 21.7 | 46.6 / 46.7 | 54.4 / 54.5 | 55.5 / 55.5 |
| sponza-flat | landscape | 22.1 / 22.0 | 45.8 / 45.8 | 53.8 / 53.9 | 54.4 / 54.6 |
| sponza-flat-fitted | portrait | 22.1 / 22.1 | 45.6 / 45.8 | 55.2 / 55.3 | 56.4 / 56.6 |
| sponza-flat-fitted | landscape | 22.7 / 22.7 | 44.4 / 44.5 | 53.5 / 53.6 | 54.8 / 54.9 |
| capybara | portrait | 47.6 / 47.5 | 21.0 / 21.1 | 21.3 / 21.3 | 21.9 / 22.0 |
| capybara | landscape | 46.3 / 46.3 | 21.6 / 21.6 | 21.8 / 21.9 | 21.8 / 21.9 |
| capybara-front | portrait | 48.5 / 48.4 | 20.6 / 20.6 | 20.9 / 21.0 | 21.6 / 21.4 |
| capybara-front | landscape | 47.5 / 47.4 | 21.0 / 21.1 | 21.3 / 21.3 | 21.3 / 21.3 |
| capybara-side | portrait | 48.4 / 48.3 | 20.6 / 20.7 | 20.9 / 21.0 | 21.4 / 21.5 |
| capybara-side | landscape | 46.9 / 46.9 | 21.3 / 21.3 | 21.6 / 21.5 | 21.6 / 21.6 |
| capybara-back | portrait | 49.6 / 50.0 | 20.1 / 20.0 | 20.5 / 20.3 | 21.3 / 21.1 |
| capybara-back | landscape | 48.2 / 48.1 | 20.7 / 20.8 | 21.1 / 21.0 | 21.2 / 21.1 |
| capybara-close | portrait | 44.0 / 44.0 | 22.7 / 22.8 | 23.0 / 23.0 | 23.6 / 23.5 |
| capybara-close | landscape | 42.5 / 42.4 | 23.5 / 23.6 | 23.7 / 23.8 | 23.8 / 23.9 |
| capybara-blend | portrait | 46.4 / 46.4 | 21.5 / 21.6 | 22.3 / 22.4 | 22.3 / 22.4 |
| capybara-blend | landscape | 45.1 / 45.1 | 22.2 / 22.2 | 22.5 / 22.4 | 22.5 / 22.4 |
| bistro-fitted-full | portrait | 23.6 / 23.5 | 42.3 / 42.3 | 62.7 / 62.8 | 67.0 / 67.2 |
| bistro-fitted-full | landscape | 22.7 / 22.6 | 44.6 / 44.6 | 60.7 / 60.8 | 63.8 / 63.9 |

### Frame peaks along the path

Board 1. Each scene's played frames over path time, its frame peaks ranked,
above what the cull counted at poses replayed paused along the path.

![sponza-fitted-full: frames along the path and the cull's counts](images/frame-peaks/sponza-fitted-full.png)
![sponza-fitted: frames along the path and the cull's counts](images/frame-peaks/sponza-fitted.png)
![sponza: frames along the path and the cull's counts](images/frame-peaks/sponza.png)
![sponza-lite: frames along the path and the cull's counts](images/frame-peaks/sponza-lite.png)
![sponza-flat: frames along the path and the cull's counts](images/frame-peaks/sponza-flat.png)
![sponza-flat-fitted: frames along the path and the cull's counts](images/frame-peaks/sponza-flat-fitted.png)
![capybara: frames along the path and the cull's counts](images/frame-peaks/capybara.png)
![capybara-front: frames along the path and the cull's counts](images/frame-peaks/capybara-front.png)
![capybara-side: frames along the path and the cull's counts](images/frame-peaks/capybara-side.png)
![capybara-back: frames along the path and the cull's counts](images/frame-peaks/capybara-back.png)
![capybara-close: frames along the path and the cull's counts](images/frame-peaks/capybara-close.png)
![capybara-blend: frames along the path and the cull's counts](images/frame-peaks/capybara-blend.png)
![bistro-fitted-full: frames along the path and the cull's counts](images/frame-peaks/bistro-fitted-full.png)

#### Frame peaks, ranked

One played frame per peak along the looping path, its slowest, ranked by wall time. A peak counts
when it stands 8% of its height above the higher of its two saddles.
Stage ms and counts come from replaying each pose paused;
Why compares them with the median of the 64 poses spaced evenly along the path (1662 of 1664 read).

| Scene | Quarter | Rank | Path s | Frame ms | Prominence ms | r3d.cull ms | r3d.transform ms | r3d.draw ms | r3d.upscale ms | Clusters tested / kept / hidden / drawn | Triangles submitted / set up / cover / miss / away | Why |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|
| sponza | portrait | 1 | 0.07 | 67.8 | 28.4 | 1.35 | 7.31 | 43.84 | 2.59 | 636 / 588 / 146 / 365 | 14153 / 7089 / 2638 / 3966 / 2808 | r3d.draw +7.5 ms; triangles missed ×1.76 median |
| sponza | portrait | 2 | 17.68 | 62.5 | 21.8 | 1.32 | 5.72 | 41.69 | 2.61 | 477 / 400 / 97 / 271 | 10382 / 5464 / 2431 / 2405 / 2081 | r3d.draw +5.3 ms; triangles set up ×1.19 median |
| sponza | landscape | 1 | 0.07 | 65.0 | 26.4 | 1.36 | 7.17 | 41.98 | 2.66 | 631 / 583 / 146 / 373 | 13913 / 7069 / 2612 / 3673 / 2834 | r3d.draw +7.7 ms; triangles facing away ×1.74 median |
| sponza | landscape | 2 | 17.90 | 60.3 | 18.6 | 1.34 | 5.68 | 39.73 | 2.59 | 466 / 399 / 108 / 263 | 10384 / 5389 / 2322 / 2437 / 2063 | r3d.draw +5.5 ms; triangles facing away ×1.27 median |
| sponza-lite | portrait | 1 | 2.33 | 53.9 | 18.4 | 1.11 | 4.30 | 34.25 | 2.64 | 324 / 297 / 48 / 229 | 6955 / 3543 / 1729 / 1434 / 1432 | r3d.draw +4.6 ms; triangles facing away ×1.52 median |
| sponza-lite | portrait | 2 | 17.56 | 51.9 | 15.5 | 1.10 | 3.77 | 33.12 | 2.65 | 269 / 224 / 53 / 162 | 5778 / 2833 / 1476 / 1115 / 1139 | r3d.draw +3.4 ms; triangles facing away ×1.21 median |
| sponza-lite | landscape | 1 | 0.06 | 52.3 | 17.0 | 1.09 | 4.48 | 32.18 | 2.62 | 331 / 317 / 54 / 242 | 7386 / 3884 / 1759 / 1588 / 1578 | r3d.draw +3.9 ms; triangles facing away ×1.78 median |
| sponza-lite | landscape | 2 | 18.00 | 49.8 | 11.5 | 1.05 | 3.78 | 31.28 | 2.62 | 258 / 221 / 47 / 163 | 5677 / 2860 / 1466 / 1107 / 1060 | r3d.draw +3.0 ms; triangles facing away ×1.19 median |
| sponza-flat | portrait | 1 | 2.40 | 55.5 | 21.9 | 1.32 | 5.60 | 34.36 | 2.59 | 529 / 469 / 109 / 330 | 12836 / 6523 / 2599 / 3206 / 2657 | r3d.draw +6.8 ms; clusters drawn ×1.53 median |
| sponza-flat | portrait | 2 | 18.15 | 51.4 | 16.3 | 1.34 | 4.85 | 31.38 | 2.60 | 436 / 358 / 82 / 260 | 10350 / 5477 / 2399 / 2369 / 2101 | r3d.draw +3.8 ms; clusters drawn ×1.20 median |
| sponza-flat | landscape | 1 | 0.06 | 54.4 | 21.6 | 1.32 | 5.80 | 32.91 | 2.60 | 544 / 498 / 114 / 342 | 13674 / 7076 / 2618 / 3512 / 2838 | r3d.draw +6.0 ms; triangles facing away ×1.74 median |
| sponza-flat | landscape | 2 | 17.88 | 50.7 | 14.4 | 1.36 | 4.86 | 30.64 | 2.53 | 422 / 352 / 88 / 248 | 10233 / 5428 / 2343 / 2326 / 2082 | r3d.draw +3.8 ms; triangles facing away ×1.27 median |
| sponza-fitted | portrait | 1 | 0.06 | 53.9 | 20.7 | 1.12 | 4.63 | 33.34 | 2.72 | 334 / 316 / 45 / 242 | 7059 / 4449 / 2177 / 1506 / 948 | r3d.draw +4.8 ms; triangles facing away ×1.87 median |
| sponza-fitted | portrait | 2 | 17.58 | 50.5 | 14.1 | 1.08 | 4.06 | 31.94 | 2.71 | 277 / 228 / 43 / 172 | 5491 / 3366 / 2014 / 1101 / 632 | r3d.draw +3.4 ms; clusters hidden ×1.26 median |
| sponza-fitted | landscape | 1 | 0.06 | 51.1 | 16.8 | 1.12 | 4.50 | 30.84 | 2.75 | 330 / 313 / 50 / 246 | 6959 / 4442 / 2148 / 1378 / 981 | r3d.draw +4.1 ms; triangles facing away ×1.97 median |
| sponza-fitted | landscape | 2 | 17.89 | 47.4 | 12.9 | 1.07 | 3.96 | 28.62 | 2.70 | 274 / 223 / 36 / 174 | 5336 / 3322 / 1927 / 1103 / 604 | r3d.draw +1.9 ms; triangles facing away ×1.21 median |
| sponza-fitted-full | portrait | 1 | 0.07 | 68.9 | 32.4 | 1.59 | 7.53 | 44.59 | 2.55 | 643 / 600 / 124 / 414 | 13710 / 8450 / 3615 / 3431 / 1714 | r3d.draw +8.2 ms; clusters hidden ×2.03 median |
| sponza-fitted-full | portrait | 2 | 17.56 | 61.7 | 21.0 | 1.28 | 5.92 | 40.16 | 2.59 | 478 / 409 / 67 / 320 | 9993 / 6458 / 3499 / 2160 / 1163 | r3d.draw +3.8 ms; triangles facing away ×1.21 median |
| sponza-fitted-full | landscape | 1 | 0.07 | 65.4 | 27.3 | 1.59 | 7.52 | 41.56 | 2.54 | 640 / 597 / 133 / 422 | 13553 / 8415 / 3555 / 3245 / 1769 | r3d.draw +7.2 ms; clusters hidden ×2.16 median |
| sponza-fitted-full | landscape | 2 | 18.26 | 58.2 | 19.4 | 1.26 | 5.88 | 37.27 | 2.54 | 459 / 402 / 62 / 321 | 9831 / 6417 / 3454 / 2083 / 1098 | r3d.draw +2.9 ms; triangles covering ×1.21 median |
| sponza-flat-fitted | portrait | 1 | 0.06 | 56.4 | 25.1 | 1.32 | 6.22 | 34.26 | 2.54 | 554 / 512 / 108 / 369 | 13889 / 8483 / 3583 / 3446 / 1752 | r3d.draw +7.0 ms; clusters hidden ×2.16 median |
| sponza-flat-fitted | portrait | 2 | 17.82 | 51.7 | 18.0 | 1.31 | 5.26 | 31.21 | 2.61 | 419 / 360 / 60 / 285 | 10091 / 6591 / 3575 / 2093 / 1199 | r3d.draw +4.0 ms; triangles set up ×1.21 median |
| sponza-flat-fitted | landscape | 1 | 0.11 | 54.8 | 22.2 | 1.33 | 6.22 | 32.96 | 2.58 | 544 / 515 / 115 / 366 | 13866 / 8516 / 3509 / 3358 / 1775 | r3d.draw +7.0 ms; clusters hidden ×2.32 median |
| sponza-flat-fitted | landscape | 2 | 17.93 | 49.3 | 16.5 | 1.22 | 5.09 | 29.30 | 2.57 | 416 / 356 / 56 / 278 | 9953 / 6388 / 3367 / 2114 / 1196 | r3d.draw +3.3 ms; triangles facing away ×1.21 median |
| capybara | portrait | 1 | 0.02 | 21.9 | 1.2 | 0.12 | 0.30 | 5.81 | 2.45 | 32 / 32 / 11 / 21 | 994 / 449 / 329 / 21 / 522 | r3d.cull +0.0 ms; triangles missed ×2.47 median |
| capybara | landscape | 1 | 0.17 | 21.8 | 0.5 | 0.14 | 0.32 | 6.22 | 2.61 | 32 / 32 / 8 / 21 | 994 / 432 / 330 / 8 / 552 | r3d.cull +0.0 ms; triangles facing away ×1.03 median |
| capybara-front | portrait | 1 | 0.02 | 21.6 | 1.1 | 0.12 | 0.30 | 5.39 | 2.49 | 32 / 32 / 9 / 22 | 994 / 495 / 285 / 46 / 451 | r3d.cull +0.0 ms; triangles missed ×1.21 median |
| capybara-front | landscape | 1 | 0.26 | 21.3 | 0.5 | 0.13 | 0.29 | 5.97 | 2.73 | 32 / 32 / 11 / 21 | 994 / 493 / 307 / 39 / 460 | r3d.upscale +0.1 ms; clusters hidden ×1.10 median |
| capybara-side | portrait | 1 | 0.02 | 21.4 | 1.1 | 0.12 | 0.30 | 5.51 | 2.55 | 32 / 32 / 8 / 22 | 994 / 415 / 303 / 12 / 565 | r3d.upscale +0.1 ms; clusters hidden ×1.14 median |
| capybara-side | landscape | 1 | 0.02 | 21.6 | 0.5 | 0.12 | 0.29 | 6.18 | 2.77 | 32 / 32 / 8 / 22 | 994 / 415 / 302 / 12 / 565 | r3d.upscale +0.1 ms; clusters hidden ×1.14 median |
| capybara-back | portrait | 1 | 0.02 | 21.3 | 1.5 | 0.12 | 0.30 | 5.01 | 2.47 | 32 / 32 / 10 / 18 | 994 / 294 / 156 / 12 / 686 | r3d.draw +0.1 ms; clusters hidden ×1.11 median |
| capybara-back | landscape | 1 | 0.02 | 21.2 | 0.8 | 0.12 | 0.30 | 5.62 | 2.60 | 32 / 32 / 9 / 19 | 994 / 293 / 159 / 11 / 688 | r3d.cull +0.0 ms; triangles facing away ×1.02 median |
| capybara-close | portrait | 1 | 0.03 | 23.6 | 1.2 | 0.14 | 0.29 | 7.31 | 2.55 | 32 / 31 / 10 / 19 | 962 / 349 / 235 / 109 / 502 | r3d.cull +0.0 ms; clusters hidden ×1.25 median |
| capybara-close | landscape | 1 | 0.03 | 23.8 | 0.6 | 0.14 | 0.29 | 8.25 | 2.60 | 32 / 31 / 10 / 19 | 962 / 360 / 247 / 64 / 536 | r3d.draw +0.0 ms; clusters hidden ×1.25 median |
| capybara-blend | portrait | 1 | 0.02 | 22.3 | 1.3 | 0.12 | 0.29 | 5.83 | 2.58 | 32 / 32 / 11 / 21 | 994 / 449 / 329 / 21 / 522 | r3d.upscale +0.1 ms; triangles missed ×2.33 median |
| capybara-blend | landscape | 1 | 0.02 | 22.5 | 1.0 | 0.12 | 0.30 | 6.34 | 2.58 | 32 / 32 / 11 / 21 | 994 / 454 / 325 / 15 / 523 | r3d.draw +0.0 ms; triangles missed ×1.67 median |
| bistro-fitted-full | portrait | 1 | 59.16 | 67.0 | 42.8 | 1.27 | 8.19 | 43.85 | 2.57 | 688 / 658 / 260 / 330 | 12662 / 6729 / 2710 / 3048 / 2839 | r3d.draw +18.2 ms; triangles set up ×1.94 median |
| bistro-fitted-full | portrait | 2 | 46.94 | 63.5 | 9.0 | 1.62 | 8.76 | 38.89 | 2.52 | 846 / 700 / 219 / 355 | 13695 / 5515 / 2722 / 5197 / 2981 | r3d.draw +13.3 ms; triangles missed ×2.67 median |
| bistro-fitted-full | portrait | 3 | 4.68 | 61.9 | 31.6 | 1.59 | 10.24 | 35.79 | 2.51 | 887 / 852 / 473 / 245 | 16585 / 6833 / 2125 / 5175 / 4516 | r3d.draw +10.2 ms; clusters hidden ×3.38 median |
| bistro-fitted-full | portrait | 4 | 80.46 | 51.2 | 9.5 | 1.19 | 7.16 | 28.77 | 2.71 | 628 / 564 / 375 / 128 | 10885 / 5008 / 946 / 2854 / 2906 | r3d.draw +3.2 ms; clusters hidden ×2.68 median |
| bistro-fitted-full | portrait | 5 | 92.28 | 50.0 | 9.5 | 1.22 | 6.48 | 28.65 | 2.60 | 632 / 500 / 244 / 179 | 9588 / 3901 / 1161 / 2800 / 2781 | r3d.draw +3.0 ms; triangles facing away ×1.85 median |
| bistro-fitted-full | portrait | 6 | 99.17 | 44.3 | 14.0 | 0.97 | 4.51 | 24.66 | 2.43 | 329 / 295 / 25 / 244 | 6071 / 3475 / 2093 / 1692 / 612 | r3d.upscale -0.1 ms; triangles covering ×1.36 median |
| bistro-fitted-full | portrait | 7 | 121.19 | 35.6 | 9.2 | 0.86 | 2.53 | 18.44 | 2.58 | 159 / 114 / 9 / 99 | 2332 / 1632 / 1392 / 527 / 50 | r3d.upscale +0.1 ms; triangles covering ×0.90 median |
| bistro-fitted-full | landscape | 1 | 46.79 | 63.8 | 37.0 | 1.57 | 8.50 | 39.09 | 2.55 | 803 / 684 / 232 / 362 | 13336 / 5638 / 2863 / 4704 / 2992 | r3d.draw +11.5 ms; triangles missed ×2.25 median |
| bistro-fitted-full | landscape | 2 | 61.18 | 63.2 | 9.8 | 1.34 | 8.62 | 39.14 | 2.53 | 790 / 696 / 298 / 315 | 13439 / 6785 / 2516 / 3674 / 2885 | r3d.draw +11.6 ms; clusters hidden ×1.92 median |
| bistro-fitted-full | landscape | 3 | 4.23 | 61.7 | 29.5 | 1.50 | 10.40 | 35.58 | 2.55 | 887 / 854 / 493 / 241 | 16664 / 6788 / 1997 / 5250 / 4562 | r3d.draw +8.0 ms; clusters hidden ×3.17 median |
| bistro-fitted-full | landscape | 4 | 54.17 | 60.6 | 4.9 | 1.54 | 7.42 | 38.32 | 2.53 | 746 / 584 / 229 / 285 | 11223 / 5060 / 2347 / 3737 / 2416 | r3d.draw +10.8 ms; triangles missed ×1.79 median |
| bistro-fitted-full | landscape | 5 | 92.37 | 54.5 | 10.6 | 1.24 | 7.03 | 33.60 | 2.63 | 636 / 549 / 275 / 211 | 10533 / 4623 / 1525 / 2393 / 3412 | r3d.draw +6.1 ms; triangles facing away ×2.05 median |
| bistro-fitted-full | landscape | 6 | 79.91 | 53.8 | 8.3 | 1.22 | 7.28 | 32.10 | 2.69 | 644 / 581 / 385 / 135 | 11182 / 5235 / 927 / 2769 / 3041 | r3d.draw +4.6 ms; clusters hidden ×2.48 median |
| bistro-fitted-full | landscape | 7 | 98.95 | 45.1 | 12.8 | 0.93 | 4.45 | 25.68 | 2.48 | 329 / 293 / 30 / 249 | 6037 / 3620 / 2053 / 1417 / 712 | r3d.upscale -0.1 ms; clusters drawn ×1.30 median |
| bistro-fitted-full | landscape | 8 | 119.35 | 36.2 | 7.2 | 0.78 | 2.63 | 19.22 | 2.59 | 148 / 119 / 6 / 102 | 2421 / 1545 / 1303 / 717 / 15 | r3d.upscale +0.0 ms; triangles covering ×0.79 median |

Sources:

- `frame-profile-1-board.log`
- `frame-profile-2-board.log`
<!-- /generated: frame-timing-profiles -->
