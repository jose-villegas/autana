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

## On the board

A development build answers these from any shell, so a measurement selects
its scene by name rather than through the menu:

| Command | Does |
|---|---|
| `autana render scenes` | every scene's key and name, and which one is showing |
| `autana render scene <key>` | switches to the scene with exactly that key, such as `sponza` or `sponza-lite` |
| `autana render partial on\|off` | partial updates, as the menu's toggle sets them |
| `autana tune render_lab.scale <n>` | the fixed render scale in hundredths of the panel: 200 is half size |
| `autana tune render_lab.budget <ms>` | dynamic resolution on a lit-mesh scene; 0 turns it off |

A screenshot's state carries an `app` object naming the scene, whether the
menu is open, the layout, partial updates and the scale, so two captures can
be checked to have measured the same thing.

## Images in the docs

`doc_images.sh` here makes these in `docs/images/overview/` and `docs/images/render/`, run by
`launcher/tools/render/render_doc_images.sh`; see "Images in these docs" in
[`docs/tools/Render-Harness.md`](../../../../../docs/tools/Render-Harness.md).

| Image | Shows |
|---|---|
| `render-lab-cube.png`, `render-lab-cube.gif` | the Gouraud cube; the GIF plays the rotation forward and back |
| `render-lab-cornell.png` | the ray-traced Cornell box, fully resolved, no HUD |
| `render-lab-sponza.gif` | the start of the Sponza flythrough, on the fitted full mesh |
| `render/sponza-{full,lite,flat,fitted,fitted-full}.gif` | the same three seconds of the flythrough, one GIF per bake |
| `render/sponza-{depth,tiles}.gif` | those three seconds as the depth and depth-tile views of the full bake |
| `render/bake-fidelity-sheet.png` | the flat bake against the source model at two poses, with the error heatmap ([Bake-Quality.md](../../../../../docs/render/Bake-Quality.md#fidelity-against-the-source)) |
| `render/bake-indirect-compare.png`, `render/bake-indirect-crops.png` | the physical reference beside the smooth bake without and with indirect light (the scene's physical look, bakes made without and with that field), each with its error heatmap against the reference at two poses, then the places the two bakes differ most with the reference above them ([Bake-Quality.md](../../../../../docs/render/Bake-Quality.md#indirect-light)) |
| `render/bake-indirect-look.png` | the physical reference beside the indirect bake at intensity 1, 2 and 3 and at an albedo boost of 2, each with its error heatmap, then each look's own reference and the error against it ([Bake-Quality.md](../../../../../docs/render/Bake-Quality.md#indirect-look)) |
| `render/bake-ao-compare.png`, `render/bake-ao-crops.png`, `render/bake-ao-map.png` | the reference beside the smooth bake without and with local occlusion at two poses with error heatmaps, the places they differ most, and the occlusion factor alone beside the reference ([Bake-Quality.md](../../../../../docs/render/Bake-Quality.md#local-occlusion)) |
| `render/compare-full-{lite,flat}.png`, `.crops.png` | full against lite and smooth against flat at the GIFs' last pose: both renders and their difference, then the places they differ most, enlarged |
| `render/compare-lite-fitted.png`, `.crops.png` | lite against the fitted mesh at that pose, the same way |
| `render/compare-full-fitted-full.png`, `.crops.png` | full against the fitted full mesh, the same way |
| `render/appearance-{chosen,fitted-full}-heat.png`, `-reference.crops.png` | each fitted mesh against the reference: its heatmap sheet and the places it differs most (fitted full also its `-reference.png` sheet) |
| `render/import-light.png`, `render/import-face-samples.png` | CPU albedo against baked light, and fixed face sampling against adaptive |
| `render/gpu/*.png` | GPU recipe comparisons, path-cull differences, normal heatmaps and the budget/cost Pareto sheet |

## Refreshing the bake comparisons

The measured comparisons of bakes are in
[Bake-Quality.md](../../../../../docs/render/Bake-Quality.md); these commands
regenerate its images and tables. The scenes `sponza`, `sponza-lite`,
`sponza-flat`, `sponza-fitted` and `sponza-fitted-full` each draw one variant.
`tests/test_sky_through_walls.py` flies the full, flat and lite bakes and fails
when more frames show sky through a wall than its ceiling allows.
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

![Lite against fitted](../../../../../docs/images/render/compare-lite-fitted.png)
![Lite against fitted, the places they differ most](../../../../../docs/images/render/compare-lite-fitted.crops.png)

The fitted mesh has lite's budget, moved off what the flythrough never draws
and fitted to the reference: arches, shadow edges and the banners' colours
come back.

![Full against fitted full](../../../../../docs/images/render/compare-full-fitted-full.png)
![Full against fitted full, the places they differ most](../../../../../docs/images/render/compare-full-fitted-full.crops.png)

The fitted full mesh is the same recipe at full's budget, so the same edges
and colours come back on full's finer geometry.

## Fidelity against the source

The source model is lit per pixel at the same camera-path poses as the
doc images. The generated fidelity table scores the committed bakes against
that reference. Metric definitions are in
[Mesh-Import.md](../../../../../docs/render/Mesh-Import.md#fidelity-against-a-reference).
`doc_images.sh` owns the poses and measurement commands.

<!-- generated: sponza-fidelity sha256=09072737b39c15733a636e9d2ea727fc55fa4ef392bde6f6e0ef50d34dc6cc83 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth | 9.101 | 28.245 | 0.5931 | 16.799 | 7.397 |
| Lite smooth | 11.040 | 34.597 | 0.5363 | 19.361 | 9.179 |
| Flat, committed | 12.412 | 41.872 | 0.4454 | 22.516 | 10.152 |
<!-- /generated: sponza-fidelity -->

The flat and smooth bakes differ in how colour varies across a face. The
generated comparison scores the same fidelity poses. The sheet and enlarged
crops in [The Sponza variants](#the-sponza-variants)
show where that difference lies.

<!-- generated: sponza-flat-smooth sha256=3bdb5845938187516b8a3c18f2477d00fe6e07b396f793349c642e29eb1d02d9 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 9.798 | 33.485 | 0.5455 | 17.543 | 8.473 |
<!-- /generated: sponza-flat-smooth -->

The flat sampling sweep re-bakes the current scene over the same geometry
and scores it against the same reference. Rows are sorted by mean error.
Labels beginning with min or max change the auto bounds; area scales the
median face area; sky changes the sky-ray count. Sampling changes bake
quality without adding work to the runtime renderer.

<!-- generated: sponza-flat-sampling sha256=e5cd81eac799755a4468a7c458ff7ef6b0122200169a5208bd2d7d890943d78f -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed64 | 10.973 | 33.694 | 0.5114 | 19.312 | 9.108 |
| fixed32 | 11.005 | 33.681 | 0.5100 | 19.374 | 9.133 |
| fixed16 | 11.064 | 34.118 | 0.5038 | 19.485 | 9.179 |
| fixed8 | 11.216 | 34.422 | 0.4950 | 19.861 | 9.289 |
| area0.25 | 11.483 | 36.892 | 0.4861 | 20.781 | 9.400 |
| min4 | 11.673 | 37.135 | 0.4723 | 20.672 | 9.658 |
| fixed4 | 11.703 | 37.395 | 0.4685 | 20.713 | 9.687 |
| area0.5 | 11.941 | 40.082 | 0.4679 | 21.869 | 9.716 |
| min2 | 12.107 | 39.445 | 0.4550 | 21.582 | 9.989 |
| declared | 12.412 | 41.872 | 0.4454 | 22.516 | 10.152 |
| max32 | 12.413 | 41.872 | 0.4452 | 22.515 | 10.153 |
| max8 | 12.420 | 41.871 | 0.4447 | 22.527 | 10.160 |
| sky256 | 12.424 | 41.845 | 0.4449 | 22.500 | 10.170 |
| sky512 | 12.430 | 41.847 | 0.4447 | 22.509 | 10.175 |
| sky64 | 12.437 | 41.833 | 0.4451 | 22.505 | 10.184 |
| fixed2 | 12.459 | 42.246 | 0.4396 | 21.875 | 10.362 |
| max4 | 12.474 | 42.230 | 0.4421 | 22.537 | 10.222 |
| sky32 | 12.533 | 41.878 | 0.4436 | 22.547 | 10.289 |
| sky16 | 12.845 | 41.872 | 0.4408 | 22.570 | 10.660 |
| area2 | 13.032 | 45.316 | 0.4211 | 23.397 | 10.721 |
| centroid | 13.069 | 45.968 | 0.4077 | 23.342 | 10.774 |
| fixed1 | 13.549 | 46.618 | 0.3931 | 23.817 | 11.261 |
<!-- /generated: sponza-flat-sampling -->

The sheet of the committed flat bake, left to right the reference,
the bake, the ΔE heatmap and the reference's edge pixels (magenta), with the
heatmap's scale below. The error sits at lit arch edges, shadow boundaries and
the foreground drapery. `doc_images.sh` regenerates the sheet.

![Reference, flat bake, error heatmap and edge pixels](../../../../../docs/images/render/bake-fidelity-sheet.png)

### Appearance fit of the lite and full meshes

The GPU stage rebuilds GI bakes and fitted meshes from the scene's current
recipes. It scores every mesh against the same held-out reference poses;
triangle counts come from the output meshes. The sheets include reference
heatmaps and enlarged differences. The generated comparison below reports
appearance, normal error, path culling and predicted time. GPU fits are scratch
recipe outputs; the board table measures the committed scene assets.

<!-- generated: sponza-gpu sha256=cabd0fa7f1aaa3a811d7f9f1ad14aee76af86c3a7e3afb8c39838076e3d43227 -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| lite-GI-bake | 8670 | 10.997 | 33.754 | 0.540 | 26.210 | 45.711 |
| lite-GI-fit | 8672 | 5.631 | 15.285 | 0.751 | 15.606 | 45.890 |
| full-GI-bake | 17374 | 9.033 | 27.568 | 0.599 | 21.154 | 57.443 |
| full-path-culled | 11974 | 9.025 | 27.501 | 0.600 | 19.118 | 51.326 |
| full-GI-fit | 17287 | 5.143 | 13.330 | 0.788 | 13.248 | 57.478 |

![lite GI bake and fit](../../../../../docs/images/render/gpu/appearance-indirect-lite.png)
![lite GI bake and fit.crops](../../../../../docs/images/render/gpu/appearance-indirect-lite.crops.png)
![full GI bake and fit](../../../../../docs/images/render/gpu/appearance-indirect-full.png)
![full GI bake and fit.crops](../../../../../docs/images/render/gpu/appearance-indirect-full.crops.png)

![Full bake and path cull](../../../../../docs/images/render/gpu/appearance-path-culled.png)

![Path cull differences](../../../../../docs/images/render/gpu/appearance-path-culled.crops.png)
<!-- /generated: sponza-gpu -->

### Budget and normal sweeps

The budget sweep varies the lite recipe's pruning budget and cost weight.
The full recipe fit is included as its own point. The Pareto sheet plots
held-out appearance against predicted time. These
predictions use the cost weights; refresh the board stage before interpreting
them as a model of current hardware performance.

<!-- generated: sponza-budget sha256=4e3f19668b2237e46b767acda02498ee741999f7b89db3b435ad39166c90a530 -->
| Budget | Cost weight | Triangles | Held-out dE76 | Predicted ms |
|---|---|---|---|---|
| 4000 | 0.0 | 4000 | 5.918 | 37.585 |
| 4000 | 0.1 | 4000 | 6.031 | 36.153 |
| 6000 | 0.0 | 6000 | 5.721 | 41.321 |
| 6000 | 0.1 | 6000 | 5.885 | 39.002 |
| 8672 | 0.0 | 8672 | 5.631 | 45.890 |
| 8672 | 0.1 | 8672 | 5.793 | 41.890 |
| 17381 | 0.0 | 17287 | 5.143 | 57.478 |

![Budget and cost sweep](../../../../../docs/images/render/gpu/appearance-pareto.png)
<!-- /generated: sponza-budget -->

The normal sweep varies the normal term while retaining the lite recipe's
other settings. The angle heatmaps show where geometry differs from the source.

<!-- generated: sponza-normal sha256=1a897c33d7ded4ba3df4b04ead0a2082d547e93e620f8e6051c1b03a24f13e6d -->
| Normal weight | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| normal-0 | 8672 | 5.622 | 15.169 | 0.751 | 19.802 | 45.447 |
| normal-0.1 | 8672 | 5.609 | 15.152 | 0.751 | 18.576 | 45.465 |
| normal-0.3 | 8672 | 5.639 | 15.205 | 0.750 | 17.618 | 45.559 |
| normal-1 | 8672 | 5.631 | 15.285 | 0.751 | 15.606 | 45.890 |

![Normal angle heatmaps](../../../../../docs/images/render/gpu/appearance-normal-heat.png)
<!-- /generated: sponza-normal -->

### Board measurements

The board stage consumes captures from one firmware commit, takes the median
of each variant's mean across captures, and fits cost weights from the full
and lite per-pose timings. Its generated model table records the source rows.
It does not access the board.

<!-- generated: sponza-board sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-board -->

<!-- generated: sponza-board-model sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-board-model -->

### Refresh commands

The import-light and face-sampling examples are regenerated by the CPU stage
with its albedo bake and flat-sampling sweep.

The `doc-images-gpu` workflow runs the full GPU stage on the self-hosted
Linux GPU runner and opens or updates its own refresh PR,
"docs: refresh GPU-rendered images", on `feature/refresh-doc-images-gpu`.
It runs weekly, on manual dispatch, and on main pushes affecting its inputs.

For a local run from Git Bash, use the documented WSL Ubuntu CUDA environment
with both r3d requirements files installed. Host C/C++ compilers are needed
for scoring. Run one GPU job at a time. The guard checks memory at stage
start; later workers are admitted against live memory, as described in
[Render-Harness](../../../../../docs/tools/Render-Harness.md#images-in-these-docs).

```sh
DOC_PROJECT=$(pwd -W)
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 --cd "$DOC_PROJECT" -- bash -lc 'sh launcher/tools/render/run_doc_gpu.sh'
MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 --cd "$DOC_PROJECT" -- bash -lc 'sh launcher/tools/render/run_doc_gpu.sh --check'
```

`--smoke` fits a few steps on a small reference set and writes scratch data
only. It cannot update or check doc images. Full output is under
`launcher/tools/results/doc_images/out/gpu/`; GPU images publish to
`docs/images/render/gpu/`. CPU refreshes neither check nor remove GPU output.
`--check` verifies the full stage's saved source stamp, tables and pixels,
without training a second stochastic fit.

Collect the perf-suite captures from the same diagnostics image, retaining
its build identity in each capture. Pass one `--capture` per run:

```sh
python launcher/tools/render/doc_stages.py --stage board --capture /path/to/run1.txt --capture /path/to/run2.txt
python launcher/tools/render/doc_stages.py --stage board --capture /path/to/run1.txt --capture /path/to/run2.txt --check
```

The stage rejects missing variants, per-pose timings, failed suites and
mismatched build identities. `--build-commit SHA` accepts a capture from that commit only when the firmware
sources still match; this lets a documentation-only commit retain its captures.
It rewrites the board tables and
`launcher/tools/r3d/board_cost_weights.txt`. Refresh GPU predictions after
changing those weights. Board readings for scratch budget-sweep meshes need
a firmware capture of those meshes; predicted ms is labelled separately.

## Sponza poses

The flythrough is a glTF camera animation, `../assets/flythrough.glb`, named
by `../assets/flythrough.anim.toml`. Its poses for
[`tools/r3d/report_triangle_sizes.sh`](../../../../tools/r3d/README.md#triangle-sizes)
come from [`tools/anim/track_host.py`](../../../../tools/anim/README.md),
which runs the device's track sampler over the clip, at the poses
`suite_sponza_perf.c` times (every `SPONZA_POSE_EVERY_MS`) and the size
`sponza_content.h` names and the lens of the scene's camera object
(`meshes/sponza.scene.toml`):

```sh
python launcher/tools/anim/track_host.py \
    launcher/main/apps/render_lab/assets/flythrough.anim.toml \
    --every 5000 --poses camera 184 224 0.62 6 |
    ./launcher/tools/r3d/report_triangle_sizes.sh \
        --mesh sponza.atrium -
```

## The capybara asset

`../assets/capybara.blend` is a hand-modelled low-poly capybara (992
triangles, 22 deform bones) with a control rig and five in-place loops at
30 fps: `idle`, `walk`, `walk_fast`, `gallop` and `half_bound`. It is the source
asset for skinned-mesh import; nothing in the build reads it yet.

`../assets/capybara.glb` is its glTF export: deform bones only, every loop as
an animation, four influences per vertex. Host tools that read glTF use it,
such as the [skinned-mesh lighting](../../../../../docs/render/Skinned-Lighting.md)
measurement. After editing the `.blend`, export it again with Blender
through the model-agnostic exporter, naming the five loops (the file also
holds the rig's own `capyrigAction`):

```sh
blender --background --factory-startup --python launcher/tools/gltf/blend_skin_to_glb.py -- \
    launcher/main/apps/render_lab/assets/capybara.blend launcher/main/apps/render_lab/assets/capybara.glb \
    --clips idle,walk,walk_fast,gallop,half_bound
```

and measure the lighting again:

```sh
launcher/tools/r3d/skin_light/report_skin_light.sh launcher/main/apps/render_lab/assets/capybara.glb gallop
```
