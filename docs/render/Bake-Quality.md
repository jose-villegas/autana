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
| ![Sponza flythrough, flat fitted](../images/render/sponza-flat-fitted.gif) | **Flat fitted**: flat's budget, one colour per face, its vertices and face colours fitted together | `sponza.atrium_flat_fitted` |

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

![Flat against flat fitted](../images/render/compare-flat-flat-fitted.png)
![Flat against flat fitted, the places they differ most](../images/render/compare-flat-flat-fitted.crops.png)

The flat fitted mesh keeps one colour per face but fits those colours and its
positions to the reference: the banners, arches and floor take their colours
back.

## Fidelity against the source

The source model is lit per pixel at the same camera-path poses as the
doc images. The generated fidelity table scores the committed bakes against
that reference. Metric definitions are in
[Mesh-Import.md](Mesh-Import.md#fidelity-against-a-reference).

<!-- generated: sponza-fidelity sha256=cdbc60a0d8ea838c832e326f792968caab1d059a880d5fba334a9b163c644b9f -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth | 9.520 | 28.182 | 0.5890 | 16.468 | 7.951 |
| Lite smooth | 11.507 | 34.225 | 0.5270 | 19.775 | 9.682 |
| Flat, committed | 12.858 | 45.665 | 0.4398 | 22.573 | 10.660 |
<!-- /generated: sponza-fidelity -->

The flat and smooth bakes differ in how colour varies across a face. The
generated comparison scores the same fidelity poses. The sheet and enlarged
crops in [The Sponza variants](#the-sponza-variants)
show where that difference lies.

<!-- generated: sponza-flat-smooth sha256=5c3eb8a7b8aec4442145f75ab5c8bed0a55235fef80736e3dd6b2482551ed98b -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 10.436 | 37.104 | 0.5383 | 17.743 | 9.265 |
<!-- /generated: sponza-flat-smooth -->

The flat sampling sweep re-bakes the current scene over the same geometry
and scores it against the same reference. Rows are sorted by mean error.
Labels beginning with min or max change the auto bounds; area scales the
median face area; sky changes the sky-ray count. Sampling changes bake
quality without adding work to the runtime renderer.

<!-- generated: sponza-flat-sampling sha256=5e83facf3c6289ed9f0f15e90bfbd21495e66bddd57f81c85ee5bd5ed4f8257d -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed32 | 11.284 | 37.047 | 0.5083 | 19.090 | 9.531 |
| fixed64 | 11.286 | 36.860 | 0.5086 | 19.023 | 9.543 |
| fixed16 | 11.390 | 36.881 | 0.5029 | 19.322 | 9.606 |
| fixed8 | 11.558 | 37.621 | 0.4903 | 19.604 | 9.739 |
| area0.25 | 11.736 | 38.507 | 0.4881 | 20.320 | 9.783 |
| min4 | 11.891 | 40.242 | 0.4753 | 20.190 | 10.009 |
| fixed4 | 11.933 | 40.107 | 0.4729 | 20.286 | 10.042 |
| area0.5 | 12.254 | 43.218 | 0.4661 | 21.540 | 10.150 |
| min2 | 12.392 | 43.656 | 0.4545 | 21.378 | 10.378 |
| fixed2 | 12.654 | 43.730 | 0.4439 | 21.689 | 10.618 |
| max4 | 12.833 | 45.683 | 0.4388 | 22.594 | 10.628 |
| max32 | 12.857 | 45.665 | 0.4398 | 22.573 | 10.660 |
| declared | 12.858 | 45.665 | 0.4398 | 22.573 | 10.660 |
| max8 | 12.859 | 45.667 | 0.4395 | 22.586 | 10.660 |
| sky64 | 12.880 | 45.671 | 0.4401 | 22.575 | 10.688 |
| sky512 | 12.883 | 45.634 | 0.4394 | 22.564 | 10.691 |
| sky256 | 12.887 | 45.661 | 0.4391 | 22.565 | 10.696 |
| sky32 | 12.919 | 45.667 | 0.4393 | 22.584 | 10.731 |
| sky16 | 13.229 | 45.651 | 0.4361 | 22.619 | 11.099 |
| area2 | 13.346 | 46.867 | 0.4229 | 23.276 | 11.094 |
| centroid | 13.552 | 48.025 | 0.4125 | 23.352 | 11.325 |
| fixed1 | 13.904 | 48.032 | 0.3969 | 23.423 | 11.739 |
<!-- /generated: sponza-flat-sampling -->

The sheet of the committed flat bake, left to right the reference,
the bake, the ΔE heatmap and the reference's edge pixels (magenta), with the
heatmap's scale below. The error sits at lit arch edges, shadow boundaries and
the foreground drapery.

![Reference, flat bake, error heatmap and edge pixels](../images/render/bake-fidelity-sheet.png)

### Appearance fit of the lite and full meshes

The fit is appearance-driven simplification<sup>[[1]](../Citations.md#1)</sup>;
[Against the paper](Scene-Files.md#against-the-paper) lists what it changes.

The GPU stage rebuilds GI bakes and fitted meshes from the scene's current
recipes. It scores every mesh against the same held-out reference poses;
triangle counts come from the output meshes. The sheets include reference
heatmaps and enlarged differences. The generated comparison below reports
appearance, normal error, path culling and predicted time. GPU fits are scratch
recipe outputs; the board table measures the committed scene assets.

<!-- generated: sponza-gpu sha256=df6eafef57b7191a0c98045fca294f84ff1a63d32f9a8f5cdff39ec4457af267 -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| lite-GI-bake | 8669 | 11.474 | 34.101 | 0.527 | 25.000 | 47.347 |
| lite-GI-fit | 8672 | 5.713 | 15.486 | 0.747 | 15.467 | 45.662 |
| full-GI-bake | 17367 | 9.550 | 28.167 | 0.593 | 21.313 | 58.788 |
| full-path-culled | 10806 | 9.547 | 28.139 | 0.593 | 19.338 | 49.165 |
| full-GI-fit | 17177 | 5.240 | 13.621 | 0.783 | 13.225 | 56.602 |

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

<!-- generated: sponza-budget sha256=5b34c15eb3945ce2fa70048f1e75d5e2eefbfdaf2bdfb22fcbae78d01a5dbae8 -->
| Budget | Cost weight | Triangles | Held-out dE76 | Predicted ms |
|---|---|---|---|---|
| 4000 | 0.0 | 4000 | 5.939 | 37.790 |
| 4000 | 0.1 | 4000 | 6.027 | 36.271 |
| 6000 | 0.0 | 6000 | 5.785 | 41.269 |
| 6000 | 0.1 | 6000 | 5.867 | 38.911 |
| 8672 | 0.0 | 8672 | 5.713 | 45.662 |
| 8672 | 0.1 | 8672 | 5.840 | 41.802 |
| 17381 | 0.0 | 17177 | 5.240 | 56.602 |

![Budget and cost sweep](../images/render/gpu/appearance-pareto.png)
<!-- /generated: sponza-budget -->

The normal sweep varies the normal term while retaining the lite recipe's
other settings. The angle heatmaps show where geometry differs from the source.

<!-- generated: sponza-normal sha256=6bfac8e1c0202550d09a6e68e1700f09cd3f213e4a48f569dd9f562dea0904f0 -->
| Normal weight | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| normal-0 | 8672 | 5.699 | 15.471 | 0.748 | 18.768 | 45.399 |
| normal-0.1 | 8672 | 5.694 | 15.320 | 0.750 | 17.917 | 45.503 |
| normal-0.3 | 8672 | 5.686 | 15.268 | 0.751 | 16.917 | 45.450 |
| normal-1 | 8672 | 5.713 | 15.486 | 0.747 | 15.467 | 45.662 |

![Normal angle heatmaps](../images/render/gpu/appearance-normal-heat.png)
<!-- /generated: sponza-normal -->

### Appearance fit of the flat mesh

[A flat fit](Scene-Files.md#a-flat-fit) fits a colour per triangle and the
positions. The GPU stage scores it against the flat bake and the smooth fit at
lite's budget, over the same held-out poses. Predicted time is left out: the
cost model has no shading term.

<!-- generated: sponza-flat-fit sha256=fdffe052ac40b60f1bf57a764e80b4e6d42c066fbeb30e99b977e1444b1da8bb -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle |
|---|---|---|---|---|---|
| flat-GI-bake | 17367 | 12.956 | 45.400 | 0.440 | 21.313 |
| lite-GI-fit | 8672 | 5.713 | 15.486 | 0.747 | 15.467 |
| flat-GI-fit | 17177 | 5.149 | 14.178 | 0.769 | 13.478 |

![Flat bake, lite fit and flat fit](../images/render/gpu/appearance-indirect-flat.png)
![Flat bake, lite fit and flat fit.crops](../images/render/gpu/appearance-indirect-flat.crops.png)
<!-- /generated: sponza-flat-fit -->

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

<!-- generated: sponza-indirect sha256=c58a07591bf0d1a889ae24a02a6211ff994ccaeb9fbfa6e10b954f3b2c504452 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Full smooth, indirect light | 8.068 | 25.340 | 0.6455 | 16.769 | 6.418 |
| Lite smooth, indirect light | 9.844 | 32.271 | 0.5915 | 20.094 | 7.920 |
| Flat, indirect light | 10.559 | 41.386 | 0.5393 | 22.105 | 8.352 |
| Full smooth, direct light | 10.795 | 28.125 | 0.5919 | 18.395 | 9.344 |
| Lite smooth, direct light | 11.997 | 32.718 | 0.5521 | 21.327 | 10.237 |
| Flat, direct light | 12.173 | 39.854 | 0.5377 | 22.951 | 10.107 |
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

<!-- generated: sponza-indirect-look sha256=5ae464fe0568c8888fb04f993cd8d0908abcf80ad8c7cafe5955843415a20aae -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 10.795 | | 28.125 | 0.5919 |
| intensity 1 | 8.068 | 8.068 | 25.340 | 0.6455 |
| intensity 2 | 8.883 | 9.306 | 25.806 | 0.6318 |
| intensity 3 | 10.710 | 10.274 | 28.864 | 0.6039 |
| albedo boost 2 | 9.479 | 9.617 | 27.154 | 0.6213 |
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
