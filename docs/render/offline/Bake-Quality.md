# Bake Quality

What each bake option buys, measured on the Sponza atrium along its flythrough
against the source model lit per pixel. How a bake is made and scored is
[Bake-Method.md](Bake-Method.md); the settings are in [Scene-Files.md](../scene/Scene-Files.md).
Every image, chart and table here is regenerated from the source
([Render-Harness.md](../../tools/Render-Harness.md#images-in-these-docs)).

## The demo variants

Every row plays the same stretch of the flythrough, so the rows compare. The
rows are ranked best first: fitted variants, then the rest, each by mean ΔE in
[Fidelity against the source](#fidelity-against-the-source).

<!-- generated: sponza-variants sha256=7ae1119ef7b9f837c787153cbaa4b4cf4174e8cb85f256af13c4df438602ba7e -->
| Flythrough | Variant | What it is | Mesh entry | Fitted |
|---|---|---|---|---|
| ![fitted full flythrough](../../images/render/sponza-fitted-full.gif) | **fitted full** | The same recipe at full's budget | `sponza.atrium_fitted_full` | yes |
| ![flat fitted flythrough](../../images/render/sponza-flat-fitted.gif) | **flat fitted** | Flat's budget, one colour per face, its vertices and face colours fitted together | `sponza.atrium_flat_fitted` | yes |
| ![fitted flythrough](../../images/render/sponza-fitted.gif) | **fitted** | Lite's budget spent on what the flythrough draws, its vertices and colours fitted to the reference | `sponza.atrium_fitted` | yes |
| ![lite flythrough](../../images/render/sponza-lite.gif) | **lite** | The same bake simplified to a smaller budget | `sponza.atrium_lite` | no |
| ![flat flythrough](../../images/render/sponza-flat.gif) | **flat** | The full mesh's triangles, one colour per face, no gradients | `sponza.atrium_flat` | no |
| ![full flythrough](../../images/render/sponza-full.gif) | **full** | Smooth: one colour per vertex, lit and interpolated | `sponza.atrium` | no |
<!-- /generated: sponza-variants -->

Where the variants differ, at the pose the GIFs end on, in the same order: each
sheet is the two renders and their amplified difference, and the crops below
it enlarge the places that differ most.

<!-- generated: sponza-variant-pairs sha256=4189bb6df1e9532974502b74e340edec3c2bdd38fd8dfacc00e7d48d05b83deb -->
| Pair | What changes |
|---|---|
| full against fitted full | The fitted recipe on full's finer geometry |
| flat against flat fitted | Face colours and positions fitted: banners, arches and floor take their colours back |
| lite against fitted | Same budget, moved to what the flythrough draws and fitted: arches, shadow edges and banner colours come back |
| fitted full against fitted | The same fit recipe on fewer triangles: small shapes merge or drop and edges step |
| full against lite | Fewer triangles: small shapes merge or drop and edges step; surfaces keep their colour |
| full against flat | One colour per face: a curtain's fold reads as bands |

![full against fitted full](../../images/render/compare-full-fitted-full.png)
![full against fitted full, the places they differ most](../../images/render/compare-full-fitted-full.crops.png)
![flat against flat fitted](../../images/render/compare-flat-flat-fitted.png)
![flat against flat fitted, the places they differ most](../../images/render/compare-flat-flat-fitted.crops.png)
![lite against fitted](../../images/render/compare-lite-fitted.png)
![lite against fitted, the places they differ most](../../images/render/compare-lite-fitted.crops.png)
![fitted full against fitted](../../images/render/compare-fitted-full-fitted.png)
![fitted full against fitted, the places they differ most](../../images/render/compare-fitted-full-fitted.crops.png)
![full against lite](../../images/render/compare-full-lite.png)
![full against lite, the places they differ most](../../images/render/compare-full-lite.crops.png)
![full against flat](../../images/render/compare-full-flat.png)
![full against flat, the places they differ most](../../images/render/compare-full-flat.crops.png)
<!-- /generated: sponza-variant-pairs -->

## Fidelity against the source

The committed bakes scored against the source lit per pixel at the camera-path
poses; the scores are defined in
[Bake-Method.md](Bake-Method.md#fidelity-against-a-reference). Fewer
triangles and one colour per face both cost most at edges; fitting brings both back.

![Colour error and structure of every committed variant, best first](../../images/render/sponza-fidelity.png)

<details>
<summary>Fidelity scores</summary>

<!-- generated: sponza-fidelity sha256=12600afa31b8e86250baa3d78febd05a3b3c8a9a13d39447d88193797d11ff4b -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fitted full | 34.603 | 65.969 | 0.3150 | 37.959 | 33.957 |
| flat fitted | 35.655 | 67.638 | 0.2919 | 38.766 | 35.048 |
| fitted | 37.869 | 64.921 | 0.3942 | 38.509 | 37.780 |
| lite | 31.097 | 62.135 | 0.3082 | 35.367 | 30.280 |
| flat | 31.329 | 65.566 | 0.2161 | 35.396 | 30.576 |
| full | 33.059 | 62.933 | 0.3083 | 36.607 | 32.382 |
<!-- /generated: sponza-fidelity -->

</details>

What the fit alone buys, each bake beside its fitted variant with the same
shading: the reference, the bake, the fitted bake, then each one's ΔE heatmap
on one colour scale. Flat's error sits at lit arch edges, shadow boundaries
and the foreground drapery, full's at arch and banner edges; both fits keep a
thin trace at edges.

![Reference, flat, flat fitted, and each bake's error heatmap on one scale](../../images/render/bake-fidelity-sheet.png)

![Reference, full, fitted full, and each bake's error heatmap on one scale](../../images/render/bake-fidelity-full-sheet.png)

Flat against smooth at the same poses, the colour gradient across each face
alone:

<!-- generated: sponza-flat-smooth sha256=5c99e8068d450370728d529710025b4ff2fc9b42f64028b7b2f385543045fa4f -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 17.726 | 57.143 | 0.6102 | 16.282 | 17.806 |
<!-- /generated: sponza-flat-smooth -->

The flat sampling sweep re-bakes the flat mesh with other face samples and
scores it against the same reference. It changes bake quality only, not
runtime work.

| Label | Changes |
|---|---|
| `declared` | The committed flat bake's own settings, the "flat" row above |
| `fixedN` | N samples on every face |
| `minN`, `maxN` | The automatic count's lower or upper bound |
| `areaX` | The automatic count's area per sample, times the median face area |
| `skyN` | The sky light's ray count |
| `centroid` | One sample at each face's centroid |

![Mean and p95 error of each flat sampling variant from 0, best first, coloured by kind](../../images/render/sponza-flat-sampling.png)

<details>
<summary>Every sampling variant</summary>

<!-- generated: sponza-flat-sampling sha256=2cc585852dfa79b7fba7f0af055101bd4f782185d687e8ee34e1c88cc874d882 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed8 | 29.810 | 62.374 | 0.2914 | 33.830 | 29.052 |
| fixed32 | 29.889 | 62.755 | 0.2973 | 33.942 | 29.122 |
| fixed64 | 29.895 | 62.818 | 0.2982 | 33.976 | 29.124 |
| fixed16 | 30.042 | 62.941 | 0.2960 | 34.218 | 29.259 |
| area0.25 | 30.094 | 62.306 | 0.2808 | 34.217 | 29.330 |
| area0.5 | 30.138 | 63.369 | 0.2627 | 34.903 | 29.276 |
| fixed4 | 30.190 | 63.387 | 0.2694 | 34.575 | 29.372 |
| min4 | 30.353 | 63.303 | 0.2679 | 34.671 | 29.551 |
| area2 | 30.430 | 66.557 | 0.2256 | 36.182 | 29.337 |
| centroid | 30.655 | 66.493 | 0.2228 | 36.209 | 29.605 |
| max4 | 30.696 | 65.194 | 0.2498 | 35.611 | 29.777 |
| fixed1 | 30.768 | 66.401 | 0.2152 | 36.057 | 29.770 |
| declared | 31.006 | 65.559 | 0.2492 | 35.853 | 30.108 |
| max32 | 31.006 | 65.559 | 0.2492 | 35.853 | 30.108 |
| max8 | 31.007 | 65.559 | 0.2492 | 35.853 | 30.108 |
| sky512 | 31.011 | 65.519 | 0.2499 | 35.810 | 30.122 |
| sky256 | 31.016 | 65.519 | 0.2499 | 35.816 | 30.127 |
| sky64 | 31.086 | 65.261 | 0.2466 | 35.699 | 30.235 |
| sky32 | 31.094 | 65.256 | 0.2470 | 35.665 | 30.251 |
| sky16 | 31.204 | 65.172 | 0.2446 | 35.605 | 30.391 |
| min2 | 31.358 | 64.877 | 0.2626 | 35.548 | 30.577 |
| fixed2 | 31.367 | 64.704 | 0.2553 | 35.463 | 30.602 |
<!-- /generated: sponza-flat-sampling -->

</details>

### Appearance fit of the lite and full meshes

The fit is appearance-driven simplification<sup>[[1]](../../Citations.md#hasselgren-2021)</sup>;
[Against the paper](Appearance-Fit.md#against-the-paper) lists what it changes.
The GPU stage rebuilds the bakes and fitted meshes from the scene's recipes and
scores them against held-out poses. These are scratch outputs; the board table
measures the committed assets.

<!-- generated: sponza-gpu sha256=1da636602bfd562d2f142f80850216d699361b8316814d387538f3f7be50e2f5 -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| lite-GI-bake | 8669 | 11.475 | 34.114 | 0.527 | 25.000 | 47.347 |
| lite-GI-fit | 8447 | 6.149 | 17.167 | 0.718 | 18.442 | 46.589 |
| full-GI-bake | 17367 | 9.550 | 28.163 | 0.593 | 21.313 | 58.788 |
| full-path-culled | 10808 | 9.546 | 28.134 | 0.593 | 19.339 | 49.191 |
| full-GI-fit | 17023 | 5.399 | 14.198 | 0.769 | 15.369 | 57.791 |

![lite GI bake and fit](../../images/render/gpu/appearance-indirect-lite.png)
![lite GI bake and fit.crops](../../images/render/gpu/appearance-indirect-lite.crops.png)
![full GI bake and fit](../../images/render/gpu/appearance-indirect-full.png)
![full GI bake and fit.crops](../../images/render/gpu/appearance-indirect-full.crops.png)

![Full bake and path cull](../../images/render/gpu/appearance-path-culled.png)

![Path cull differences](../../images/render/gpu/appearance-path-culled.crops.png)
<!-- /generated: sponza-gpu -->

### Budget and normal sweeps

The budget sweep varies the lite recipe's pruning budget and cost weight, with
the full fit as its own point: held-out error against predicted time. The
predictions use the board's cost weights, as fresh as its last board stage.

<!-- generated: sponza-budget sha256=d24dbe491f9e5cdd759bbf336bf27299a59f842650051a3a4aa9ff9ffe7db515 -->
| Budget | Cost weight | Triangles | Held-out dE76 | Predicted ms |
|---|---|---|---|---|
| 4000 | 0.0 | 4000 | 6.554 | 38.083 |
| 4000 | 0.1 | 4000 | 6.773 | 37.257 |
| 6000 | 0.0 | 6000 | 6.182 | 42.052 |
| 6000 | 0.1 | 6000 | 6.264 | 40.842 |
| 8672 | 0.0 | 8447 | 6.149 | 46.589 |
| 8672 | 0.1 | 8672 | 6.218 | 44.627 |
| 17381 | 0.0 | 17023 | 5.399 | 57.791 |

![Budget and cost sweep](../../images/render/gpu/appearance-pareto.png)
<!-- /generated: sponza-budget -->

The normal sweep varies only the lite recipe's normal term; the heatmaps show
where the geometry's normals leave the source's.

<!-- generated: sponza-normal sha256=cd112a4ff23d765fdc93cf0a6e8feb955ab447acb3c17a2d0ff84b270619e846 -->
| Normal weight | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle | Predicted ms |
|---|---|---|---|---|---|---|
| normal-0 | 8522 | 6.127 | 17.154 | 0.718 | 20.998 | 46.400 |
| normal-0.1 | 8530 | 6.134 | 17.166 | 0.716 | 20.500 | 46.419 |
| normal-0.3 | 8452 | 6.145 | 17.311 | 0.717 | 19.997 | 46.419 |
| normal-1 | 8447 | 6.149 | 17.167 | 0.718 | 18.442 | 46.589 |

![Normal angle heatmaps](../../images/render/gpu/appearance-normal-heat.png)
<!-- /generated: sponza-normal -->

### Appearance fit of the flat mesh

[A flat fit](Appearance-Fit.md#a-flat-fit) fits a colour per triangle and the
positions. The GPU stage scores it against the flat bake and the smooth fit at
lite's budget, over the same held-out poses. Predicted time is left out: the
cost model has no shading term.

<!-- generated: sponza-flat-fit sha256=d4ea53dd762a0772ce62ff176e19fa13bd303c6ff0a263ad7fa96c5478a2cee9 -->
| Mesh | Triangles | Mean dE76 | p95 dE76 | SSIM | Normal angle |
|---|---|---|---|---|---|
| flat-GI-bake | 17367 | 12.956 | 45.403 | 0.440 | 21.313 |
| lite-GI-fit | 8447 | 6.149 | 17.167 | 0.718 | 18.442 |
| flat-GI-fit | 16960 | 5.294 | 14.709 | 0.756 | 15.534 |

![Flat bake, lite fit and flat fit](../../images/render/gpu/appearance-indirect-flat.png)
![Flat bake, lite fit and flat fit.crops](../../images/render/gpu/appearance-indirect-flat.crops.png)
<!-- /generated: sponza-flat-fit -->

### Board measurements

Each variant's frame cost along the flythrough, timed on the board: the median
of each variant's mean over captures of one firmware commit. The full and lite
per-pose timings fit the cost weights the predicted times above use.

<!-- generated: sponza-board sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-board -->

<!-- generated: sponza-board-model sha256=004310de23d1d3ede4be5737fea3df96589330524e3c9e8b073803d2c099c13d -->
Run the documented stage to populate this comparison from current inputs.
<!-- /generated: sponza-board-model -->

## Indirect light

The scene's [bake-indirect settings](../scene/Scene-Files.md#bake-indirect) lift the
shadowed arcade ceilings and the column sides the sun misses, and tint a column
beside a banner with its colour. The renderer draws the precomputed colours.

Direct against indirect bakes of each mesh, scored against the source lit per
pixel with physical bounced light. All are bakes of the physical look, the
scene without its `[indirect]` table and occlusion.

![Mean and p95 error and SSIM of the direct and indirect bakes per mesh](../../images/render/sponza-indirect.png)

<details>
<summary>Indirect light scores</summary>

<!-- generated: sponza-indirect sha256=26f3f51db6bec8bd4d822ec02144111d12b07298214c81cd6b2e713b65b849a7 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| lite, direct light | 27.088 | 60.870 | 0.3545 | 35.585 | 25.652 |
| lite, indirect light | 29.209 | 61.767 | 0.3433 | 35.427 | 28.117 |
| flat, direct light | 27.180 | 65.035 | 0.2931 | 36.021 | 25.692 |
| flat, indirect light | 26.727 | 64.524 | 0.2811 | 35.319 | 25.295 |
| full, direct light | 30.183 | 63.429 | 0.3475 | 38.059 | 28.854 |
| full, indirect light | 31.819 | 62.549 | 0.3413 | 37.364 | 30.863 |
<!-- /generated: sponza-indirect -->

</details>

The reference beside the scene's standard mesh fitted with direct and with
indirect light, each with its ΔE heatmap against the reference.

![Reference, direct-light bake and indirect bake, with error heatmaps](../../images/render/gpu/bake-indirect-compare.png)

The places the two fits differ most, the reference above them.

![Where bounce light changes the picture](../../images/render/gpu/bake-indirect-compare.crops.png)

| Where | Why bounce barely shows |
|---|---|
| Sunlit floor under a curtain | Direct light dominates; the small coloured term vanishes in the tone map and RGB565 |
| Shaded column | Bounce can exceed direct light, but both stay near black |
| Below a triangle | The reference resolves bounce detail finer than the mesh's vertex or face spacing |

The bounce itself is checked in linear light by a floor beside a sunlit wall
in [`test_r3d_path_bake.py`](../../../launcher/tools/tests/test_r3d_path_bake.py).
A Sponza bounce measurement needs the alpha-masked source and linear radiance,
not encoded vertex colours.

### Indirect look

The scene's [`[indirect]` table](../scene/Scene-Files.md#indirect) sets `intensity`, a
multiplier on bounced light, and `albedo_boost`, a multiplier on the
reflectance bounces use. The reference reads it too, so each look is scored
twice: against the physical reference (look and bake error together) and
against its own (bake error alone). The sheet shows the physical reference
above the bakes and each look's own reference below.
The sheet fits the standard mesh in each look, at the last held-out pose.

![Physical reference and each look's own, with the bakes and their error heatmaps](../../images/render/gpu/bake-indirect-look.png)

![Mean error of each look against the physical reference and its own; direct light has no look of its own, so one bar](../../images/render/sponza-indirect-look.png)

<details>
<summary>Look scores</summary>

<!-- generated: sponza-indirect-look sha256=fca60a78abf35dde2cc9299f1f2387817ad1f942fee2b4feba9b4bb2745115c4 -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 30.183 | | 63.429 | 0.3475 |
| intensity 1 | 31.819 | 31.819 | 62.549 | 0.3413 |
| intensity 2 | 32.345 | 32.741 | 62.997 | 0.3291 |
| intensity 3 | 33.803 | 34.310 | 64.199 | 0.3257 |
| albedo boost 2 | 32.656 | 32.817 | 63.666 | 0.3235 |
<!-- /generated: sponza-indirect-look -->

</details>

## Local occlusion

The scene's [`[bake].ao`](../scene/Scene-Files.md#bake-ao) scales the ambient light,
and with `indirect = true` the bounced light, by how closed in a point is.
These images start from the physical look with a brighter ambient light, so
the occlusion has something to scale. The reference applies it per pixel and
the bake per vertex.

The sheet is the reference and the standard mesh fitted without and with
occlusion, at two held-out poses, each with its error heatmap.

![Reference, bake without occlusion and bake with it, with error heatmaps](../../images/render/gpu/bake-ao-compare.png)

The places the two fits differ most, the reference above them.

![Where occlusion changes the picture](../../images/render/gpu/bake-ao-compare.crops.png)

The factor alone, beside the reference:
[Bake-Method.md](Bake-Method.md#local-occlusion-implementation).
