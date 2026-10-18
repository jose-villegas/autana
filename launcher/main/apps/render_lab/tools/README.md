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

### Height-only raster experiment

`--tune NAME=VALUE` is repeatable and uses the console tune protocol.
`render_lab.rows` is a separate height divisor in the same hundredths as
`render_lab.scale`; zero follows `scale`, and positive values below
`RENDER_LAB_SCALE_UNIT` use that unit. Defaults remain `scale=200`, `rows=0`,
`scanline=0`. The scene applies these settings each frame. Perf suites read
them when the test runs and log the raster size, effective rows divisor and
strength and effective quarter. `render_lab.perf_quarter` selects a quarter for
device performance suites; its default of -1 preserves each suite's orientation.
Their compose work uses the same half-picture predicate as the scene.

These commands render both scenes in quarter 0 and rotated quarter 1, at the
same pose used in the sheets. The host reads the real gfx panel rows, including
half-picture doubling, before rotating the output for the viewer.

```sh
sh launcher/main/apps/render_lab/tools/render_lab_render_host.sh --build-only -o /tmp/raster-look
for scene in sponza-fitted-full capybara; do
    for quarter in 0 1; do
        /tmp/raster-look/render_lab_render --scene "$scene" --quarter "$quarter" --no-hud --frames 2 --dt 5000 \
            -o "/tmp/raster-look/$scene-q$quarter-today.bmp"
        /tmp/raster-look/render_lab_render --scene "$scene" --quarter "$quarter" --no-hud --frames 2 --dt 5000 \
            --tune render_lab.rows=400 -o "/tmp/raster-look/$scene-q$quarter-plain.bmp"
        /tmp/raster-look/render_lab_render --scene "$scene" --quarter "$quarter" --no-hud --frames 2 --dt 5000 \
            --tune render_lab.rows=400 --tune render_lab.scanline=16 \
            -o "/tmp/raster-look/$scene-q$quarter-scanline.bmp"
    done
done
```

`render_compare.sh --render LABEL "ARGS"` accepts these same renderer arguments.
Use `launcher/tools/render/render_compare.py --row` with `--crops 3` to compare
default versus 1×2 plain, or plain versus strength 16. Keep captures outside the
repository. Scanline runs follow panel rows, so they appear vertical in quarter 1.
Clear sky darkens too. Strength uses every 32nd from unchanged to black.

The complete source removal inventory below matches the experiment's names
under `launcher/main/render/`, this app and `launcher/test/suites/`.
Existing `DRAW_DITHER_SCANLINES4` patterns are independent.

| File | Experiment symbols or statements to remove |
|---|---|
| `launcher/main/render/upscale.h` | `UPSCALE_SCANLINE_UNIT`, `UPSCALE_SCANLINE_MAX`, `upscale_t.scanline_strength`, `upscale_t.scanline_run` |
| `launcher/main/render/upscale.c` | strength initialization and run eligibility in `upscale_init()`, `scanline_run()`, `scanline_pixel()`, shaded paired-row calls |
| `launcher/main/gfx/target/gfx_color.h`, `gfx_target.h` | `GFX_COLOR_SCALE_UNIT`, `gfx_color_scale()`, `gfx_color_pair_scale()`, `gfx_target_paired_rows_shaded()` and its plain-copy wrapper call |
| `launcher/main/render/raster.c` | cached strength preservation in `raster_upscale()` |
| `launcher/main/render/context/render_context.h`, `render_context.c` | `render_context_set_scanlines()` |
| `launcher/main/apps/render_lab/app_render_lab.c` | `TUNE(render_lab, scanline, ...)`, `render_lab_scanline()`, the setter call in `render_lab_apply_raster_settings()` |
| `launcher/main/apps/render_lab/render_lab_view.h` | `render_lab_scanline()` declaration |
| `launcher/main/apps/render_lab/tests/suite_sponza_perf.c` | strength assignment and header field |
| `launcher/main/apps/render_lab/tests/suite_capybara_perf.c` | header strength field |
| `launcher/main/apps/render_lab/tests/suite_raster_scale_perf.c` | strength assignments, setter and header field |
| `launcher/main/apps/render_lab/tools/tests/test_render_views.py` | `COLOR_HEADER`, `SCANLINE_UNIT`, scanline tunings and dark-row assertions |
| `launcher/test/suites/suite_scene.c` | strength setting in `test_height_only_scale_composes_whole_runs_in_the_half_picture()` |
| `launcher/test/suites/suite_upscale.c` | scanline tests, `assert_scanline_rows_match_reference()` and registrations |
| `docs/Shared-Helpers.md` | regenerate the catalogue after removing the public symbols |
| `docs/render/Dynamic-Resolution.md`, this README | experiment prose and inventory |

Check the inventory with:

```sh
git grep -n -i scanline -- launcher/main/render launcher/main/gfx/target launcher/main/apps/render_lab \
    launcher/test/suites/suite_scene.c launcher/test/suites/suite_upscale.c docs/render/Dynamic-Resolution.md
```

Keep height-only fixed scaling, `raster_fits_half_picture()`, `rows`, the
general `--tune` option and plain-row regression coverage when removing the
experiment. The engine contract is in
[Dynamic resolution](../../../../../docs/render/Dynamic-Resolution.md).

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
| `autana tune render_lab.budget <ms>` | dynamic resolution on a lit-mesh scene; 0 turns it off |

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
`suite_sponza_perf.c` times (every `SPONZA_POSE_EVERY_MS`) and the size
`sponza_content.h` names and the lens of the scene's camera object
(`launcher/demo/sponza/sponza.scene.toml`):

```sh
python launcher/tools/anim/track_host.py \
    launcher/demo/sponza/flythrough.anim.toml \
    --every 5000 --poses camera 184 224 0.62 6 |
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
`--suite run_sponza_perf_suite`. Its fixed flythrough samples measure the selected
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
