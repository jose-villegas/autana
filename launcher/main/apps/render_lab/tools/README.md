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
| `render/bake-fidelity-sheet.png` | the flat bake against the source model at two poses, with the error heatmap (see Fidelity against the source) |
| `render/bake-indirect-compare.png`, `render/bake-indirect-crops.png` | the reference beside the smooth bake without and with indirect light (from a bake of the import made without that field), each with its error heatmap against the reference at two poses, then the places the two bakes differ most with the reference above them (see Indirect light) |
| `render/bake-indirect-look.png` | the reference beside the indirect bake at intensity 1, 2 and 3 and at an albedo boost of 2, each with its error heatmap, then each look's own reference and the error against it (see Indirect look) |
| `render/bake-ao-compare.png`, `render/bake-ao-crops.png`, `render/bake-ao-map.png` | the reference beside the smooth bake without and with local occlusion at two poses with error heatmaps, the places they differ most, and the occlusion factor alone beside the reference (see Local occlusion) |
| `render/compare-full-{lite,flat}.png`, `.crops.png` | full against lite and smooth against flat at the GIFs' last pose: both renders and their difference, then the places they differ most, enlarged |
| `render/compare-lite-fitted.png`, `.crops.png` | lite against the fitted mesh at that pose, the same way |
| `render/compare-full-fitted-full.png`, `.crops.png` | full against the fitted full mesh, the same way |
| `render/appearance-{chosen,fitted-full}-heat.png`, `-reference.crops.png` | each fitted mesh against the reference: its heatmap sheet and the places it differs most (fitted full also its `-reference.png` sheet) |
| `render/import-light.png`, `render/import-face-samples.png` | CPU albedo against baked light, and fixed face sampling against adaptive |
| `render/gpu/*.png` | GPU recipe comparisons, path-cull differences, normal heatmaps and the budget/cost Pareto sheet |

## The Sponza variants

The Sponza scene places renderers over the import's variants
([Scene-Files.md](../../../../../docs/render/Scene-Files.md)): it records bakes
and appearance-fit recipes, culled to the camera's path, at lite's and full's
budgets. The scenes `sponza`,
`sponza-lite`, `sponza-flat`, `sponza-fitted` and `sponza-fitted-full` each draw one. Every row plays the
same three seconds of the flythrough, so the rows compare. The depth and tile rows
are the [view modes](../../../../../docs/render/Mesh-Rendering.md#view-modes)
over the full mesh.

| Variant | What it is | Triangles and vertices |
|---|---|---|
| ![Sponza flythrough, smooth](../../../../../docs/images/render/sponza-full.gif) | **Full**: smooth, one colour per vertex, lit and interpolated | `SPONZA_TRIANGLE_COUNT`, `SPONZA_VERTEX_COUNT` |
| ![Sponza flythrough, lite](../../../../../docs/images/render/sponza-lite.gif) | **Lite**: the same bake simplified to a smaller budget | `SPONZA_LITE_TRIANGLE_COUNT`, `SPONZA_LITE_VERTEX_COUNT` |
| ![Sponza flythrough, flat](../../../../../docs/images/render/sponza-flat.gif) | **Flat**: the full mesh's triangles, one colour per face, no gradients | `SPONZA_FLAT_TRIANGLE_COUNT`, `SPONZA_FLAT_VERTEX_COUNT` |
| ![Sponza flythrough, fitted](../../../../../docs/images/render/sponza-fitted.gif) | **Fitted**: lite's budget spent on what the flythrough draws, its vertices and colours fitted to the reference | the `sponza.atrium_fitted` entry's counts |
| ![Sponza flythrough, fitted full](../../../../../docs/images/render/sponza-fitted-full.gif) | **Fitted full**: the same recipe at full's budget | the `sponza.atrium_fitted_full` entry's counts |
| ![Sponza flythrough, depth](../../../../../docs/images/render/sponza-depth.gif) | `RASTER_SHOW_DEPTH` over the full mesh | as full |
| ![Sponza flythrough, depth tiles](../../../../../docs/images/render/sponza-tiles.gif) | `RASTER_SHOW_DEPTH_TILES` over the full mesh | as full |

The counts are those of the five meshes in `meshes/`.
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

<!-- generated: sponza-fidelity sha256=2851106f42de70ca9128467d01be5754615853736e1c11e48ae3f167fc7f10dd -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth | 7.063 | 22.997 | 0.6712 | 15.404 | 5.496 |
| Lite smooth | 8.531 | 29.299 | 0.6127 | 18.088 | 6.730 |
| Flat, committed | 8.945 | 35.650 | 0.6047 | 20.275 | 6.814 |
<!-- /generated: sponza-fidelity -->

The flat and smooth bakes differ in how colour varies across a face. The
generated comparison scores the same fidelity poses. The sheet and enlarged
crops in [The Sponza variants](#the-sponza-variants)
show where that difference lies.

<!-- generated: sponza-flat-smooth sha256=e66246bae9730487b376777fe5fce66641bc683fbf3e36a29776278c70bb5ace -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 7.237 | 29.382 | 0.7319 | 16.328 | 5.973 |
<!-- /generated: sponza-flat-smooth -->

The flat sampling sweep re-bakes the current scene over the same geometry
and scores it against the same reference. Rows are sorted by mean error.
Labels beginning with min or max change the auto bounds; area scales the
median face area; sky changes the sky-ray count. Sampling changes bake
quality without adding work to the runtime renderer.

<!-- generated: sponza-flat-sampling sha256=c0f1724ee5cb22e1f6110eda9b4e79c3512bd0c5e8ed08195a9899821881ea28 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed32 | 8.175 | 29.275 | 0.6265 | 17.525 | 6.427 |
| fixed64 | 8.177 | 29.523 | 0.6276 | 17.503 | 6.433 |
| fixed16 | 8.247 | 29.307 | 0.6245 | 17.732 | 6.474 |
| fixed8 | 8.375 | 30.258 | 0.6220 | 17.996 | 6.573 |
| area0.25 | 8.468 | 30.460 | 0.6185 | 18.505 | 6.583 |
| min4 | 8.513 | 30.895 | 0.6180 | 18.504 | 6.645 |
| fixed4 | 8.589 | 31.271 | 0.6177 | 18.535 | 6.726 |
| min2 | 8.673 | 32.869 | 0.6114 | 19.175 | 6.708 |
| area0.5 | 8.759 | 33.621 | 0.6112 | 19.456 | 6.749 |
| fixed2 | 8.940 | 34.232 | 0.6024 | 19.278 | 7.003 |
| max8 | 8.943 | 35.644 | 0.6042 | 20.276 | 6.812 |
| declared | 8.945 | 35.650 | 0.6047 | 20.275 | 6.814 |
| max32 | 8.945 | 35.666 | 0.6047 | 20.276 | 6.814 |
| sky64 | 8.985 | 35.586 | 0.6040 | 20.254 | 6.865 |
| sky512 | 8.985 | 35.598 | 0.6033 | 20.267 | 6.864 |
| sky256 | 8.988 | 35.589 | 0.6036 | 20.283 | 6.864 |
| max4 | 9.043 | 36.185 | 0.6033 | 20.294 | 6.925 |
| sky32 | 9.089 | 35.646 | 0.6033 | 20.299 | 6.977 |
| area2 | 9.367 | 39.114 | 0.5949 | 20.855 | 7.210 |
| sky16 | 9.427 | 35.679 | 0.5987 | 20.331 | 7.368 |
| centroid | 9.574 | 40.915 | 0.5901 | 21.127 | 7.410 |
| fixed1 | 9.762 | 41.235 | 0.5854 | 21.266 | 7.606 |
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

<!-- generated: sponza-gpu sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-gpu -->

### Local occlusion on the lite mesh

The same stage repeats the lite mesh on a scratch copy of the scene with its
ambient light raised and [`[bake].ao`](../../../../../docs/render/Scene-Files.md#bake-ao)
on. The references carry the occlusion, so the simplified bake and the fit are
scored on the same occluded picture; the fit sees the occlusion through its
training references. The sheet's heatmaps show where each mesh keeps or loses
it.

<!-- generated: sponza-gpu-ao sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-gpu-ao -->

### Budget and normal sweeps

The budget sweep varies the lite recipe's pruning budget and cost weight.
The full recipe fit is included as its own point. The Pareto sheet plots
held-out appearance against predicted time. These
predictions use the cost weights; refresh the board stage before interpreting
them as a model of current hardware performance.

<!-- generated: sponza-budget sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-budget -->

The normal sweep varies the normal term while retaining the lite recipe's
other settings. The angle heatmaps show where geometry differs from the source.

<!-- generated: sponza-normal sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
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

## Indirect light

The Sponza scene's bake-indirect settings are recorded in
`meshes/sponza.scene.toml` and described in
[Scene-Files.md](../../../../../docs/render/Scene-Files.md#bake-indirect). It
lifts the shadowed arcade ceilings and the sides of the columns the sun does
not reach, and tints a column next to a banner with the banner's colour. The
baked variants draw precomputed colours.

The bounced light is validated in linear light by a floor beside a sunlit wall,
whose bounced term must be half the wall's radiance, in
[`test_r3d_path_bake.py`](../../../../tools/tests/test_r3d_path_bake.py). A
Sponza bounce measurement must use its alpha-masked source and linear radiance,
not source triangle counts or encoded vertex colours.

The atrium's sunlit floor beneath a curtain is direct-light dominated. Its
small coloured indirect term can disappear through the tone map and RGB565
quantization even when there is substantial bounce light elsewhere.
On a shaded column the indirect term can exceed direct light, but both terms
remain close to black. The source reference resolves those local changes more
finely than the vertex-colour mesh, so a per-pixel reference is the comparison
for a suspected colour-bleed loss.

The generated table scores the direct and indirect bakes against the source
lit per pixel with the scene's indirect light, at the same poses as the
fidelity sheet. Direct-light counterparts are rebuilt from the current scene
with its bake indirect field removed. The reference resolves bounce detail
finer than a triangle, which contributes to the remaining error.

<!-- generated: sponza-indirect sha256=f92334f03aa3da30f8f1f3535865a1e964ea20c257a3d8d15a38bdccbe234e5f -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth, indirect light | 7.063 | 22.997 | 0.6712 | 15.404 | 5.496 |
| Lite smooth, indirect light | 8.531 | 29.299 | 0.6127 | 18.088 | 6.730 |
| Flat, indirect light | 8.945 | 35.650 | 0.6047 | 20.275 | 6.814 |
| Full smooth, direct light | 8.959 | 24.208 | 0.6619 | 16.648 | 7.513 |
| Lite smooth, direct light | 10.115 | 30.166 | 0.6128 | 19.067 | 8.421 |
| Flat, direct light | 10.492 | 35.881 | 0.6171 | 20.677 | 8.589 |
<!-- /generated: sponza-indirect -->

The reference beside the smooth bake with direct and indirect light, each
bake with its ΔE heatmap against the reference and the reference's edge pixels
beside it. The error stays at silhouettes and
shadow edges; the generated table measures their contribution.

![Reference, direct-light bake and indirect bake, with error heatmaps](../../../../../docs/images/render/bake-indirect-compare.png)

The places the two bakes differ most, the reference above them: a banner's
colour on the column beside it, and the lit ceiling.

![Where bounce light changes the picture](../../../../../docs/images/render/bake-indirect-crops.png)

`doc_images.sh` regenerates the images, baking the scene without `[bake] indirect`
for the direct-light side. The source reference carries the scene's indirect light; `render_compare.py`
makes the sheets and crops against that reference.

### Fitted variants against indirect light

The GPU stage's [appearance comparison](#appearance-fit-of-the-lite-and-full-meshes)
rebuilds the GI bake and GI fit at both recipe budgets against one current
reference. Its generated sheets show the held-out error and enlarged differences.

### Indirect look

The scene's `[indirect]` table, described in
[Scene-Files.md](../../../../../docs/render/Scene-Files.md#indirect), sets
`intensity` (a multiplier on the bounced light) and `albedo_boost` (a
multiplier on the reflectance bounces use, held below 1). The committed scene
leaves both at the physical 1.0. The reference reads the same table, so each
look has two references: the physical one and one made with the look's own
settings. The sheet bakes the same import at intensity 2 and 3 and at an albedo
boost of 2 and shows, at the last pose, the physical reference above the bakes
with their heatmaps against it, then each look's own reference with the
heatmap against that:

![Physical reference and each look's own, with the bakes and their error heatmaps](../../../../../docs/images/render/bake-indirect-look.png)

The generated table scores each look against both references over the
doc-image poses. The physical-reference columns include the look's difference
from physical lighting; the own-reference column isolates bake fidelity.

<!-- generated: sponza-indirect-look sha256=36b6f491745e70b6e7f53759f98fb7535c5bc0fc2b2a50a4350b6029f1ac938f -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 8.959 | | 24.208 | 0.6619 |
| intensity 1 | 7.063 | 7.063 | 22.997 | 0.6712 |
| intensity 2 | 8.085 | 7.898 | 25.058 | 0.6473 |
| intensity 3 | 9.511 | 8.432 | 26.601 | 0.6260 |
| albedo boost 2 | 8.483 | 8.004 | 25.118 | 0.6404 |
<!-- /generated: sponza-indirect-look -->

## Local occlusion

The scene's `[bake].ao` ([Scene-Files.md](../../../../../docs/render/Scene-Files.md#bake-ao))
scales the ambient light, and with `indirect = true` the bounced light,
by how closed in a point is. The scene's own ambient is faint, so these images
raise it in both bakes and add `ao` to one of them. The reference applies the
occlusion at every pixel and the bake at every vertex, so the two heatmaps show
where the bake's occlusion helps and where it overshoots.

The sheet is the reference, the bake without occlusion and the bake with it at
two poses, each with its error heatmap against the reference.

![Reference, bake without occlusion and bake with it, with error heatmaps](../../../../../docs/images/render/bake-ao-compare.png)

The places the two bakes differ most, the reference above them.

![Where occlusion changes the picture](../../../../../docs/images/render/bake-ao-crops.png)

The occlusion factor alone at the same two poses, white where nothing is near
and dark where the surroundings close in, beside the reference frame. It is
low where stone meets stone: column bases, under arches, the creases between
walls and floor and around the pots. A curtain stays open on its visible side
because a double-sided surface takes the less occluded of its two sides.

![The occlusion factor beside the reference](../../../../../docs/images/render/bake-ao-map.png)

`doc_images.sh` regenerates the images; `reference_render.py --occlusion` writes
the factor map.

## Sponza poses

The flythrough is a glTF camera animation, `../assets/flythrough.glb`, baked
to `../meshes/flythrough_tracks_generated.c` by
[`tools/anim/bake_tracks.py`](../../../../tools/anim/README.md). Its poses for
[`tools/r3d/report_triangle_sizes.sh`](../../../../tools/r3d/README.md#triangle-sizes)
come from the generic track sampler, at the poses `suite_sponza_perf.c` times
(every `SPONZA_POSE_EVERY_MS`) and the size `sponza_flythrough.h` names and the lens of
the scene's camera object (`meshes/sponza.scene.toml`):

```sh
./launcher/tools/anim/sample_tracks.sh \
    --tracks launcher/main/apps/render_lab/meshes/flythrough_tracks_generated.c:flythrough \
    --every 5000 --poses camera 184 224 0.62 6 |
    ./launcher/tools/r3d/report_triangle_sizes.sh \
        --mesh sponza.atrium -
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
