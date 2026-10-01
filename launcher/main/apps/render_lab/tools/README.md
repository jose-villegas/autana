# Render Lab tools

Host-only scripts; the firmware build skips this folder. The render harness
itself is [`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

## Host renders

```sh
./launcher/main/apps/render_lab/tools/render_lab_render_host.sh
```

Writes every declared scene under `tools/results/render/render_lab/`:
`gouraud-landscape.bmp` is the shaded cube, `cornell-landscape.bmp` the
software ray-traced room. The integer scenes are pinned with the HUD hidden;
the HUD renders are `|nopin`, since its fps readout is a `double` printed
with `"%.1f"`, and so are the Cornell scenes, which are float throughout.
The fps text comes from the host fixture - time the board with a device
capture. The Gouraud scene rotates when stepped over several frames.

### Depth views

`--view shaded|depth|tiles` shows a lit-mesh scene's frame as the renderer
left it (`shaded`, the default), as its depth buffer, or as that depth
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
one. They are `|nopin`, like the shaded Sponza renders: the camera path is
float, so which pixels a triangle reaches can differ by compiler.
`--view` sets the tunable `render_lab.view`, so on a development build
`autana tune render_lab.view 2` switches the same views live. `--view` on a
scene with no lit mesh, an unknown name or no value fails the run.
`tests/test_render_views.py` checks the views against the shaded render.

A pose of the flythrough is `--frames` times `--dt`:

```sh
render_lab_render --scene sponza --frames 1 --dt 15000 --view depth -o depth.bmp
```

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/` and `docs/images/render/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `render-lab-cube.png`, `render-lab-cube.gif` | the Gouraud cube; the GIF plays the rotation forward and back |
| `render-lab-cornell.png` | the ray-traced Cornell box, fully resolved, no HUD |
| `render-lab-sponza.gif` | the start of the Sponza flythrough |
| `render/sponza-{full,lite,flat}.gif` | the same three seconds of the flythrough, one GIF per bake |
| `render/sponza-{depth,tiles}.gif` | those three seconds as the depth and depth-tile views of the full bake |
| `render/compare-full-{lite,flat}.png`, `.crops.png` | full against lite and smooth against flat at the GIFs' last pose: both renders and their difference, then the places they differ most, enlarged |

## The Sponza variants

One Sponza import bakes three meshes, its `[[variants]]`
([Mesh-Import.md](../../../../../docs/render/Mesh-Import.md)): the scenes
`sponza`, `sponza-lite` and `sponza-flat` each draw one. Every row plays the
same three seconds of the flythrough, so the rows compare. The last two rows
are the [view modes](../../../../../docs/render/Mesh-Rendering.md#view-modes)
over the full mesh.

| Variant | What it is | Triangles and vertices |
|---|---|---|
| ![Sponza flythrough, smooth](../../../../../docs/images/render/sponza-full.gif) | **Full**: smooth, one colour per vertex, lit and interpolated | `SPONZA_TRIANGLE_COUNT`, `SPONZA_VERTEX_COUNT` |
| ![Sponza flythrough, lite](../../../../../docs/images/render/sponza-lite.gif) | **Lite**: the same bake simplified to a smaller budget | `SPONZA_LITE_TRIANGLE_COUNT`, `SPONZA_LITE_VERTEX_COUNT` |
| ![Sponza flythrough, flat](../../../../../docs/images/render/sponza-flat.gif) | **Flat**: the full mesh's triangles, one colour per face, no gradients | `SPONZA_FLAT_TRIANGLE_COUNT`, `SPONZA_FLAT_VERTEX_COUNT` |
| ![Sponza flythrough, depth](../../../../../docs/images/render/sponza-depth.gif) | `RASTER_SHOW_DEPTH` over the full mesh | as full |
| ![Sponza flythrough, depth tiles](../../../../../docs/images/render/sponza-tiles.gif) | `RASTER_SHOW_DEPTH_TILES` over the full mesh | as full |

The counts are the macros in `sponza_mesh_generated.h`,
`sponza_lite_mesh_generated.h` and `sponza_flat_mesh_generated.h`.
`autana suite run_sponza_perf_suite` prints each variant's `both cores: mean`
line (`test_sponza_frame_cost_along_the_flythrough`). The GIFs are made by the
doc-images workflow
([Render-Harness.md](../../../../../docs/tools/Render-Harness.md#images-in-these-docs)).

Where the variants differ, at the pose the GIFs end on: each sheet is the two
renders and their amplified difference, and the crops below it are the places
that differ most, the first render above the second, enlarged.

![Full against lite](../../../../../docs/images/render/compare-full-lite.png)
![Full against lite, the places they differ most](../../../../../docs/images/render/compare-full-lite.crops.png)

Lite spends fewer triangles, so small shapes merge or drop and edges step; the
surfaces keep their colour.

![Smooth against flat](../../../../../docs/images/render/compare-full-flat.png)
![Smooth against flat, the places they differ most](../../../../../docs/images/render/compare-full-flat.crops.png)

Flat shows each face in one colour, so a curtain's fold reads as bands where
the smooth mesh blends.

## Sponza poses

The flythrough is a glTF camera animation, `../assets/flythrough.glb`, baked
to `../flythrough_tracks_generated.c` by
[`tools/anim/bake_tracks.py`](../../../../tools/anim/README.md). Its poses for
[`tools/r3d/report_triangle_sizes.sh`](../../../../tools/r3d/README.md#triangle-sizes)
come from the generic track sampler, at the poses `suite_sponza_perf.c` times
(every `SPONZA_POSE_EVERY_MS`) and the size `sponza_flythrough.h` names and the lens of
the scene's camera object (`meshes/sponza.scene.toml`):

```sh
./launcher/tools/anim/sample_tracks.sh \
    --tracks launcher/main/apps/render_lab/flythrough_tracks_generated.c:flythrough \
    --every 5000 --poses camera 184 224 0.62 6 |
    ./launcher/tools/r3d/report_triangle_sizes.sh \
        --mesh launcher/main/apps/render_lab/sponza_mesh_generated.c:sponza_mesh -
```

## The capybara test asset

`gen_capybara.py` writes `../assets/capybara.glb`, a rigged low-poly capybara
modelled entirely in code: 1336 triangles, 20 joints, and two looping clips at
30 fps, `idle` (3.5 s) and an in-place `walk` (1 s, no root motion). It is a
plain glTF 2.0 file, the input a skinned-mesh baker is tested with.

```sh
python launcher/main/apps/render_lab/tools/gen_capybara.py
python -m unittest discover -s launcher/main/apps/render_lab/tools/tests
```

The file is read back and posed with the engine's glTF tools in
[`launcher/tools/r3d/`](../../../../tools/r3d/README.md); to watch it:

```sh
python launcher/tools/r3d/gltf_preview.py launcher/main/apps/render_lab/assets/capybara.glb --gif walk --out walk.gif
```
