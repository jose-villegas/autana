# Bake Quality

What each bake option buys, measured. The import's variants, the appearance
fit, indirect light and local occlusion are each compared against the source
model lit per pixel, on the Sponza atrium along its flythrough. How a bake is
made is [Mesh-Import.md](Mesh-Import.md); the scene settings that select one
are [Scene-Files.md](Scene-Files.md). Every image and table here is
regenerated from the source by the doc-images pipeline
([Render-Harness.md](../tools/Render-Harness.md#images-in-these-docs)).

## The Sponza variants

The Sponza scene places renderers over the import's variants
([Scene-Files.md](Scene-Files.md)): it records region-culled bakes
and path-culled appearance-fit recipes, at lite's and full's
budgets. Every row plays the same stretch of the flythrough, so the rows
compare.

| Variant | What it is | Mesh entry |
|---|---|---|
| ![Sponza flythrough, smooth](../images/render/sponza-full.gif) | **Full**: smooth, one colour per vertex, lit and interpolated | `sponza.atrium` |
| ![Sponza flythrough, lite](../images/render/sponza-lite.gif) | **Lite**: the same bake simplified to a smaller budget | `sponza.atrium_lite` |
| ![Sponza flythrough, flat](../images/render/sponza-flat.gif) | **Flat**: the full mesh's triangles, one colour per face, no gradients | `sponza.atrium_flat` |
| ![Sponza flythrough, fitted](../images/render/sponza-fitted.gif) | **Fitted**: lite's budget spent on what the flythrough draws, its vertices and colours fitted to the reference | `sponza.atrium_fitted` |
| ![Sponza flythrough, fitted full](../images/render/sponza-fitted-full.gif) | **Fitted full**: the same recipe at full's budget | `sponza.atrium_fitted_full` |

Where the variants differ, at the pose the GIFs end on: each sheet is the two
renders and their amplified difference, and the crops below it are the places
that differ most, the first render above the second, enlarged.

![Full against lite](../images/render/compare-full-lite.png)
![Full against lite, the places they differ most](../images/render/compare-full-lite.crops.png)

Lite spends fewer triangles, so small shapes merge or drop and edges step; the
surfaces keep their colour.

![Smooth against flat](../images/render/compare-full-flat.png)
![Smooth against flat, the places they differ most](../images/render/compare-full-flat.crops.png)

Flat shows each face in one colour, so a curtain's fold reads as bands where
the smooth mesh blends.

![Lite against fitted](../images/render/compare-lite-fitted.png)
![Lite against fitted, the places they differ most](../images/render/compare-lite-fitted.crops.png)

The fitted mesh has lite's budget, moved off what the flythrough never draws
and fitted to the reference: arches, shadow edges and the banners' colours
come back.

![Full against fitted full](../images/render/compare-full-fitted-full.png)
![Full against fitted full, the places they differ most](../images/render/compare-full-fitted-full.crops.png)

The fitted full mesh is the same recipe at full's budget, so the same edges
and colours come back on full's finer geometry.

## Fidelity against the source

The source model is lit per pixel at the same camera-path poses as the
doc images. The generated fidelity table scores the committed bakes against
that reference. Metric definitions are in
[Mesh-Import.md](Mesh-Import.md#fidelity-against-a-reference).

<!-- generated: sponza-fidelity sha256=b29a6c8238437172006d94602ccc0a7390f682d258d2fb81fd0034f480145df4 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth | 9.329 | 27.971 | 0.5871 | 16.596 | 7.717 |
| Lite smooth | 11.071 | 34.131 | 0.5390 | 19.418 | 9.217 |
| Flat, committed | 12.633 | 43.305 | 0.4473 | 22.484 | 10.448 |
<!-- /generated: sponza-fidelity -->

The flat and smooth bakes differ in how colour varies across a face. The
generated comparison scores the same fidelity poses. The sheet and enlarged
crops in [The Sponza variants](#the-sponza-variants)
show where that difference lies.

<!-- generated: sponza-flat-smooth sha256=16217b2aa653ea025f38c3600ac2205263cfb2f5eb1d47507396dc48e4a4ac52 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 9.974 | 35.222 | 0.5536 | 17.662 | 8.760 |
<!-- /generated: sponza-flat-smooth -->

The flat sampling sweep re-bakes the current scene over the same geometry
and scores it against the same reference. Rows are sorted by mean error.
Labels beginning with min or max change the auto bounds; area scales the
median face area; sky changes the sky-ray count. Sampling changes bake
quality without adding work to the runtime renderer.

<!-- generated: sponza-flat-sampling sha256=3a39a782aa8db7f47c5ef37affdbce34b3b5e97922c603ccf5db0de0a9e43613 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed64 | 11.319 | 36.563 | 0.5036 | 19.637 | 9.464 |
| fixed32 | 11.357 | 36.804 | 0.5017 | 19.703 | 9.496 |
| fixed16 | 11.422 | 36.319 | 0.4967 | 19.812 | 9.551 |
| fixed8 | 11.595 | 36.718 | 0.4863 | 20.178 | 9.687 |
| area0.25 | 11.694 | 37.980 | 0.4834 | 20.799 | 9.666 |
| min4 | 11.972 | 39.144 | 0.4697 | 20.984 | 9.969 |
| area0.5 | 12.061 | 40.238 | 0.4683 | 21.878 | 9.877 |
| fixed4 | 12.115 | 40.294 | 0.4625 | 21.096 | 10.123 |
| min2 | 12.310 | 40.794 | 0.4578 | 21.697 | 10.222 |
| fixed2 | 12.462 | 40.639 | 0.4431 | 21.767 | 10.394 |
| max32 | 12.642 | 43.225 | 0.4467 | 22.592 | 10.431 |
| declared | 12.642 | 43.225 | 0.4467 | 22.592 | 10.431 |
| max8 | 12.644 | 43.221 | 0.4468 | 22.590 | 10.434 |
| sky64 | 12.649 | 43.205 | 0.4458 | 22.577 | 10.442 |
| sky256 | 12.652 | 43.221 | 0.4472 | 22.594 | 10.442 |
| sky512 | 12.654 | 43.231 | 0.4472 | 22.594 | 10.445 |
| max4 | 12.721 | 44.633 | 0.4427 | 22.667 | 10.515 |
| sky32 | 12.761 | 43.236 | 0.4454 | 22.621 | 10.568 |
| sky16 | 13.094 | 43.237 | 0.4423 | 22.650 | 10.963 |
| area2 | 13.552 | 48.076 | 0.4098 | 23.717 | 11.310 |
| centroid | 13.648 | 48.899 | 0.4017 | 23.810 | 11.408 |
| fixed1 | 13.953 | 48.836 | 0.3938 | 23.847 | 11.764 |
<!-- /generated: sponza-flat-sampling -->

The sheet of the committed flat bake, left to right the reference,
the bake, the ΔE heatmap and the reference's edge pixels (magenta), with the
heatmap's scale below. The error sits at lit arch edges, shadow boundaries and
the foreground drapery.

![Reference, flat bake, error heatmap and edge pixels](../images/render/bake-fidelity-sheet.png)

### Appearance fit of the lite and full meshes

The GPU stage rebuilds GI bakes and fitted meshes from the scene's current
recipes. It scores every mesh against the same held-out reference poses;
triangle counts come from the output meshes. The sheets include reference
heatmaps and enlarged differences. The generated comparison below reports
appearance, normal error, path culling and predicted time. GPU fits are scratch
recipe outputs; the board table measures the committed scene assets.

<!-- generated: sponza-gpu sha256=decf3f2d0a7a868846d254ab93e24c3921689a5ef137defdd1e4780ed0c243ad -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| lite-GI-bake | 8670 | 10.997 | 33.754 | 0.540 | 26.210 | 45.711 |
| lite-GI-fit | 8672 | 5.631 | 15.285 | 0.751 | 15.606 | 45.890 |
| full-GI-bake | 17374 | 9.033 | 27.568 | 0.599 | 21.154 | 57.443 |
| full-path-culled | 11974 | 9.025 | 27.501 | 0.600 | 19.118 | 51.326 |
| full-GI-fit | 17287 | 5.143 | 13.330 | 0.788 | 13.248 | 57.478 |

![lite GI bake and fit](../images/render/gpu/appearance-indirect-lite.png)
![lite GI bake and fit.crops](../images/render/gpu/appearance-indirect-lite.crops.png)
![full GI bake and fit](../images/render/gpu/appearance-indirect-full.png)
![full GI bake and fit.crops](../images/render/gpu/appearance-indirect-full.crops.png)

![Full bake and path cull](../images/render/gpu/appearance-path-culled.png)

![Path cull differences](../images/render/gpu/appearance-path-culled.crops.png)
<!-- /generated: sponza-gpu -->

### Budget and normal sweeps

The budget sweep varies the lite recipe's pruning budget and cost weight.
The full recipe fit is included as its own point. The Pareto sheet plots
held-out appearance against predicted time. These
predictions use the cost weights; refresh the board stage before interpreting
them as a model of current hardware performance.

<!-- generated: sponza-budget sha256=8c988c841bb09f93077965842f52473777fb63cf111b3008df3367af5f3ec2ec -->
| Budget | Cost weight | Triangles | Held-out dE76 | Predicted ms |
|---|---|---|---|---|
| 4000 | 0.0 | 4000 | 5.918 | 37.585 |
| 4000 | 0.1 | 4000 | 6.031 | 36.153 |
| 6000 | 0.0 | 6000 | 5.721 | 41.321 |
| 6000 | 0.1 | 6000 | 5.885 | 39.002 |
| 8672 | 0.0 | 8672 | 5.631 | 45.890 |
| 8672 | 0.1 | 8672 | 5.793 | 41.890 |
| 17381 | 0.0 | 17287 | 5.143 | 57.478 |

![Budget and cost sweep](../images/render/gpu/appearance-pareto.png)
<!-- /generated: sponza-budget -->

The normal sweep varies the normal term while retaining the lite recipe's
other settings. The angle heatmaps show where geometry differs from the source.

<!-- generated: sponza-normal sha256=9a803746d1962ef11dcc7dde9f1ff47786829eb22c4109593fe7644679ceb088 -->
| Normal weight | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| normal-0 | 8672 | 5.622 | 15.169 | 0.751 | 19.802 | 45.447 |
| normal-0.1 | 8672 | 5.609 | 15.152 | 0.751 | 18.576 | 45.465 |
| normal-0.3 | 8672 | 5.639 | 15.205 | 0.750 | 17.618 | 45.559 |
| normal-1 | 8672 | 5.631 | 15.285 | 0.751 | 15.606 | 45.890 |

![Normal angle heatmaps](../images/render/gpu/appearance-normal-heat.png)
<!-- /generated: sponza-normal -->

### Board measurements

Each variant's frame cost along the flythrough, timed on the board. The
board stage of the doc-images pipeline takes captures from one firmware
commit, the median of each variant's mean across them, and fits the cost
weights the predicted times above use from the full and lite per-pose timings.
Its generated model table records the source rows.

<!-- generated: sponza-board sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-board -->

<!-- generated: sponza-board-model sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-board-model -->

## Indirect light

The Sponza scene's bake-indirect settings are recorded in its scene file and
described in
[Scene-Files.md](Scene-Files.md#bake-indirect). It
lifts the shadowed arcade ceilings and the sides of the columns the sun does
not reach, and tints a column next to a banner with the banner's colour. The
baked variants draw precomputed colours.

The bounced light is validated in linear light by a floor beside a sunlit wall,
whose bounced term must be half the wall's radiance, in
[`test_r3d_path_bake.py`](../../launcher/tools/tests/test_r3d_path_bake.py). A
Sponza bounce measurement must use its alpha-masked source and linear radiance,
not source triangle counts or encoded vertex colours.

The atrium's sunlit floor beneath a curtain is direct-light dominated. Its
small coloured indirect term can disappear through the tone map and RGB565
quantization even when there is substantial bounce light elsewhere.
On a shaded column the indirect term can exceed direct light, but both terms
remain close to black. The source reference resolves those local changes more
finely than the vertex-colour mesh, so a per-pixel reference is the comparison
for a suspected colour-bleed loss.

The generated table scores the direct and indirect bakes of the full, lite and
flat meshes against the source lit per pixel with physical bounced light, at the
same poses as the fidelity sheet. All of them are bakes of the physical look,
the scene without its `[indirect]` table and its occlusion; the direct-light
counterparts also drop `[bake].indirect`. The reference resolves bounce detail
finer than a triangle, which contributes to the remaining error.

<!-- generated: sponza-indirect sha256=4f6b227e0851581439ab768daec3c149bcbb2a4c94b71615e96c4728acad6fdf -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth, indirect light | 7.955 | 26.267 | 0.6460 | 16.992 | 6.258 |
| Lite smooth, indirect light | 9.494 | 31.393 | 0.5890 | 19.638 | 7.587 |
| Flat, indirect light | 10.483 | 41.319 | 0.5421 | 22.776 | 8.175 |
| Full smooth, direct light | 10.636 | 27.605 | 0.5920 | 18.460 | 9.155 |
| Lite smooth, direct light | 11.910 | 33.968 | 0.5471 | 21.155 | 10.167 |
| Flat, direct light | 12.281 | 40.709 | 0.5345 | 22.958 | 10.273 |
<!-- /generated: sponza-indirect -->

The reference beside the smooth bake with direct and indirect light, each
bake with its ΔE heatmap against the reference and the reference's edge pixels
beside it. The error stays at silhouettes and
shadow edges; the generated table measures their contribution.

![Reference, direct-light bake and indirect bake, with error heatmaps](../images/render/bake-indirect-compare.png)

The places the two bakes differ most, the reference above them: a banner's
colour on the column beside it, and the lit ceiling.

![Where bounce light changes the picture](../images/render/bake-indirect-crops.png)

### Indirect look

The scene's `[indirect]` table, described in
[Scene-Files.md](Scene-Files.md#indirect), sets
`intensity` (a multiplier on the bounced light) and `albedo_boost` (a
multiplier on the reflectance bounces use, held below 1). The committed scene
sets its own look in that table; this sheet, like the indirect images above,
starts from the physical look, the scene without that table and without its
occlusion. The reference reads the same table, so each
look has two references: the physical one and one made with the look's own
settings. The sheet bakes the same import at the raised intensities and albedo boost
the table below names and shows, at the last pose, the physical reference above the bakes
with their heatmaps against it, then each look's own reference with the
heatmap against that:

![Physical reference and each look's own, with the bakes and their error heatmaps](../images/render/bake-indirect-look.png)

The generated table scores each look against both references over the
doc-image poses. The physical-reference columns include the look's difference
from physical lighting; the own-reference column isolates bake fidelity.

<!-- generated: sponza-indirect-look sha256=28ba1903e2443ed226d5a7d624e9cd30dc9ecf9c07221bd82219f5655e381a9a -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 10.636 | | 27.605 | 0.5920 |
| intensity 1 | 7.955 | 7.955 | 26.267 | 0.6460 |
| intensity 2 | 8.688 | 9.118 | 26.165 | 0.6264 |
| intensity 3 | 10.334 | 10.071 | 27.652 | 0.6010 |
| albedo boost 2 | 9.077 | 9.275 | 26.117 | 0.6178 |
<!-- /generated: sponza-indirect-look -->

## Local occlusion

The scene's `[bake].ao` ([Scene-Files.md](Scene-Files.md#bake-ao))
scales the ambient light, and with `indirect = true` the bounced light,
by how closed in a point is. The committed scene sets it in
its scene file, whose ambient light is faint. These images start
from the physical look instead, raise the
ambient light in both bakes, which gives the occlusion something to scale, and
add `ao` to one of them. The reference applies the
occlusion at every pixel and the bake at every vertex, so the two heatmaps show
where the bake's occlusion helps and where it overshoots.

The sheet is the reference, the bake without occlusion and the bake with it at
the fidelity poses, each with its error heatmap against the reference.

![Reference, bake without occlusion and bake with it, with error heatmaps](../images/render/bake-ao-compare.png)

The places the two bakes differ most, the reference above them.

![Where occlusion changes the picture](../images/render/bake-ao-crops.png)

The occlusion factor alone at the same poses, white where nothing is near
and dark where the surroundings close in, beside the reference frame. It is
low where stone meets stone: column bases, under arches, the creases between
walls and floor and around the pots. A curtain stays open on its visible side
because a double-sided surface takes the less occluded of its two sides.

![The occlusion factor beside the reference](../images/render/bake-ao-map.png)
