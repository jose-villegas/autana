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

<!-- generated: sponza-fidelity sha256=38de2a9526ed535ee1bdbb017b19a37f148542e8977c103494d3f16b0999d2af -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth | 9.101 | 28.242 | 0.5931 | 16.798 | 7.397 |
| Lite smooth | 11.040 | 34.597 | 0.5363 | 19.361 | 9.178 |
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

<!-- generated: sponza-flat-sampling sha256=806ea9d68e6a624d177deb10147b5a0550ea38ad33bc6a3852405139d6bb8a11 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed64 | 11.040 | 34.283 | 0.5096 | 19.462 | 9.163 |
| fixed32 | 11.060 | 34.031 | 0.5083 | 19.450 | 9.188 |
| fixed16 | 11.116 | 34.546 | 0.5038 | 19.591 | 9.226 |
| fixed8 | 11.258 | 34.665 | 0.4934 | 19.942 | 9.329 |
| area0.25 | 11.561 | 37.603 | 0.4844 | 20.939 | 9.471 |
| min4 | 11.683 | 37.344 | 0.4706 | 20.669 | 9.672 |
| fixed4 | 11.720 | 37.645 | 0.4676 | 20.698 | 9.713 |
| area0.5 | 12.002 | 40.511 | 0.4666 | 21.930 | 9.777 |
| min2 | 12.141 | 39.861 | 0.4542 | 21.631 | 10.022 |
| declared | 12.494 | 42.823 | 0.4429 | 22.691 | 10.216 |
| max32 | 12.494 | 42.823 | 0.4429 | 22.691 | 10.216 |
| fixed2 | 12.501 | 42.783 | 0.4399 | 21.973 | 10.389 |
| max8 | 12.505 | 42.803 | 0.4430 | 22.702 | 10.228 |
| sky512 | 12.524 | 42.736 | 0.4429 | 22.693 | 10.251 |
| sky64 | 12.525 | 42.828 | 0.4421 | 22.686 | 10.253 |
| sky256 | 12.527 | 42.739 | 0.4424 | 22.700 | 10.254 |
| max4 | 12.549 | 43.257 | 0.4406 | 22.721 | 10.274 |
| sky32 | 12.656 | 42.833 | 0.4419 | 22.719 | 10.404 |
| sky16 | 13.003 | 42.851 | 0.4382 | 22.738 | 10.819 |
| area2 | 13.138 | 46.215 | 0.4197 | 23.428 | 10.843 |
| centroid | 13.181 | 47.239 | 0.4064 | 23.602 | 10.863 |
| fixed1 | 13.587 | 47.571 | 0.3950 | 23.896 | 11.290 |
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

<!-- generated: sponza-gpu sha256=c89201778d046d99060fb062744dd73a39dc7325097f36e59c8ea7a6925689e3 -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| lite-GI-bake | 8670 | 10.997 | 33.754 | 0.540 | 26.210 | 45.711 |
| lite-GI-fit | 8672 | 5.618 | 15.175 | 0.752 | 15.741 | 45.791 |
| full-GI-bake | 17374 | 9.033 | 27.568 | 0.599 | 21.154 | 57.443 |
| full-path-culled | 11974 | 9.025 | 27.501 | 0.600 | 19.118 | 51.326 |
| full-GI-fit | 17287 | 5.170 | 13.436 | 0.787 | 13.279 | 57.356 |

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

<!-- generated: sponza-budget sha256=fcca5f46ea169479c268f034a5751ca48000656ebf3ae6be5838e3161a9c2924 -->
| Budget | Cost weight | Triangles | Held-out dE76 | Predicted ms |
|---|---|---|---|---|
| 4000 | 0.0 | 4000 | 5.930 | 37.621 |
| 4000 | 0.1 | 4000 | 6.042 | 36.226 |
| 6000 | 0.0 | 6000 | 5.703 | 41.400 |
| 6000 | 0.1 | 6000 | 5.872 | 38.980 |
| 8672 | 0.0 | 8672 | 5.618 | 45.791 |
| 8672 | 0.1 | 8672 | 5.764 | 41.970 |
| 17381 | 0.0 | 17287 | 5.170 | 57.356 |

![Budget and cost sweep](../images/render/gpu/appearance-pareto.png)
<!-- /generated: sponza-budget -->

The normal sweep varies the normal term while retaining the lite recipe's
other settings. The angle heatmaps show where geometry differs from the source.

<!-- generated: sponza-normal sha256=b1f9633bbdfb1a9a884d2c2613edc1046af46bd6fc26bb48b4ec97d536524084 -->
| Normal weight | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| normal-0 | 8672 | 5.655 | 15.382 | 0.750 | 19.932 | 45.506 |
| normal-0.1 | 8672 | 5.659 | 15.393 | 0.749 | 18.781 | 45.519 |
| normal-0.3 | 8672 | 5.623 | 15.182 | 0.751 | 17.828 | 45.663 |
| normal-1 | 8672 | 5.618 | 15.175 | 0.752 | 15.741 | 45.791 |

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

<!-- generated: sponza-indirect sha256=9795abf8d89c68fdc4f16711332ec9308ce0fb9599167d707ed6981a16710bf4 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth, indirect light | 7.638 | 24.734 | 0.6619 | 16.380 | 5.978 |
| Lite smooth, indirect light | 9.369 | 32.219 | 0.5932 | 19.985 | 7.364 |
| Flat, indirect light | 10.281 | 39.895 | 0.5462 | 21.977 | 8.073 |
| Full smooth, direct light | 10.595 | 27.822 | 0.5973 | 18.652 | 9.069 |
| Lite smooth, direct light | 11.779 | 33.142 | 0.5485 | 20.965 | 10.047 |
| Flat, direct light | 12.243 | 39.508 | 0.5368 | 22.919 | 10.235 |
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

<!-- generated: sponza-indirect-look sha256=465b7c4a7e9b7af27e7b600af9d42f9be35ae80a49cd0bc82271eb642e4cc6bc -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 10.595 | | 27.822 | 0.5973 |
| intensity 1 | 7.638 | 7.638 | 24.734 | 0.6619 |
| intensity 2 | 8.684 | 9.105 | 27.090 | 0.6280 |
| intensity 3 | 10.493 | 10.069 | 28.921 | 0.5936 |
| albedo boost 2 | 9.277 | 9.320 | 27.229 | 0.6203 |
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
