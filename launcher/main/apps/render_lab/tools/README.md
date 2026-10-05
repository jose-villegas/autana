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

<!-- generated: sponza-fidelity sha256=ff0c677745976917de5a2f1497911518583b7729a1621b5ae407f0755115fd31 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth | 7.128 | 23.079 | 0.6699 | 15.130 | 5.625 |
| Lite smooth | 8.360 | 27.690 | 0.6182 | 17.820 | 6.580 |
| Flat, committed | 8.625 | 31.437 | 0.6079 | 18.606 | 6.729 |
<!-- /generated: sponza-fidelity -->

The flat and smooth bakes differ in how colour varies across a face. The
generated comparison scores the same fidelity poses. The sheet and enlarged
crops in [The Sponza variants](#the-sponza-variants)
show where that difference lies.

<!-- generated: sponza-flat-smooth sha256=3e24b9ca98d167c006257a4a59b16d97aa4be2327c593e97ff9d67e1664f42a3 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 7.044 | 26.330 | 0.7308 | 15.081 | 5.939 |
<!-- /generated: sponza-flat-smooth -->

The flat sampling sweep re-bakes the current scene over the same geometry
and scores it against the same reference. Rows are sorted by mean error.
Labels beginning with min or max change the auto bounds; area scales the
median face area; sky changes the sky-ray count. Sampling changes bake
quality without adding work to the runtime renderer.

<!-- generated: sponza-flat-sampling sha256=d86a4d4504ae653eacd3496a8c1420cdaacbca0596a0dda8195c1cadd4126b04 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed64 | 8.049 | 27.156 | 0.6272 | 16.881 | 6.380 |
| fixed32 | 8.053 | 26.997 | 0.6267 | 16.880 | 6.384 |
| fixed16 | 8.099 | 27.470 | 0.6260 | 16.992 | 6.417 |
| fixed8 | 8.146 | 27.725 | 0.6244 | 17.146 | 6.444 |
| area0.25 | 8.292 | 28.560 | 0.6180 | 17.789 | 6.488 |
| min4 | 8.314 | 28.578 | 0.6166 | 17.663 | 6.539 |
| fixed4-sun-centre | 8.320 | 28.704 | 0.6162 | 17.718 | 6.536 |
| fixed4 | 8.322 | 28.587 | 0.6159 | 17.655 | 6.550 |
| area0.5 | 8.414 | 29.725 | 0.6145 | 18.324 | 6.528 |
| min2 | 8.454 | 30.197 | 0.6125 | 18.235 | 6.597 |
| sky512 | 8.617 | 31.534 | 0.6074 | 18.825 | 6.672 |
| sky256 | 8.619 | 31.534 | 0.6075 | 18.828 | 6.673 |
| max32 | 8.637 | 31.558 | 0.6076 | 18.844 | 6.692 |
| declared | 8.639 | 31.552 | 0.6075 | 18.847 | 6.693 |
| max4 | 8.646 | 31.636 | 0.6068 | 18.838 | 6.704 |
| max8 | 8.650 | 31.637 | 0.6078 | 18.877 | 6.702 |
| sun-centre | 8.651 | 31.755 | 0.6083 | 19.040 | 6.673 |
| fixed2 | 8.658 | 31.133 | 0.6080 | 18.444 | 6.801 |
| sky64 | 8.688 | 31.532 | 0.6066 | 18.839 | 6.753 |
| sky32 | 8.805 | 31.581 | 0.6057 | 18.863 | 6.886 |
| area2 | 8.887 | 33.356 | 0.6015 | 19.432 | 6.880 |
| centroid | 9.095 | 34.623 | 0.5955 | 19.613 | 7.100 |
| fixed1 | 9.113 | 34.618 | 0.5935 | 19.689 | 7.105 |
| sky16 | 9.160 | 31.631 | 0.6006 | 18.893 | 7.299 |
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

<!-- generated: sponza-indirect sha256=e28c0cadfee13a93bc0a0b57c36632c7a9cc9d522623edc0d66e3d180ef1eac9 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth, indirect light | 7.128 | 23.079 | 0.6699 | 15.130 | 5.625 |
| Lite smooth, indirect light | 8.360 | 27.690 | 0.6182 | 17.820 | 6.580 |
| Flat, indirect light | 8.625 | 31.437 | 0.6079 | 18.606 | 6.729 |
| Full smooth, direct light | 9.578 | 24.567 | 0.6597 | 16.511 | 8.282 |
| Lite smooth, direct light | 10.602 | 28.902 | 0.6148 | 18.455 | 9.123 |
| Flat, direct light | 11.110 | 36.454 | 0.6160 | 20.762 | 9.302 |
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

<!-- generated: sponza-indirect-look sha256=c51b803c340bd09475f9e871e7e46787ed473dd6e3e2703811ae387c214630e5 -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 9.578 | | 24.567 | 0.6597 |
| intensity 1 | 7.128 | 7.128 | 23.079 | 0.6699 |
| intensity 2 | 8.182 | 7.615 | 23.972 | 0.6556 |
| intensity 3 | 10.204 | 8.139 | 26.361 | 0.6275 |
| albedo boost 2 | 8.903 | 7.734 | 24.536 | 0.6487 |
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
