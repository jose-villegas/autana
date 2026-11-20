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

<!-- generated: sponza-variants sha256=d1f6cd76b8d27218f5e525449ebfb492a31d147a9fd2b4c16f4471a89908bbed -->
| Flythrough | Variant | What it is | Mesh entry | Fitted |
|---|---|---|---|---|
| ![flat fitted flythrough](../../images/render/sponza-flat-fitted.gif) | **flat fitted** | Flat's budget, one colour per face, its vertices and face colours fitted together | `sponza.atrium_flat_fitted` | yes |
| ![fitted full flythrough](../../images/render/sponza-fitted-full.gif) | **fitted full** | The same recipe at full's budget | `sponza.atrium_fitted_full` | yes |
| ![fitted flythrough](../../images/render/sponza-fitted.gif) | **fitted** | Lite's budget spent on what the flythrough draws, its vertices and colours fitted to the reference | `sponza.atrium_fitted` | yes |
| ![full flythrough](../../images/render/sponza-full.gif) | **full** | Smooth: one colour per vertex, lit and interpolated | `sponza.atrium` | no |
| ![lite flythrough](../../images/render/sponza-lite.gif) | **lite** | The same bake simplified to a smaller budget | `sponza.atrium_lite` | no |
| ![flat flythrough](../../images/render/sponza-flat.gif) | **flat** | The full mesh's triangles, one colour per face, no gradients | `sponza.atrium_flat` | no |
<!-- /generated: sponza-variants -->

Where the variants differ, at the pose the GIFs end on, in the same order: each
sheet is the two renders and their amplified difference, and the crops below
it enlarge the places that differ most.

<!-- generated: sponza-variant-pairs sha256=ce424c79c7ffdc9806184532f604152a17715f9dfc2e9a2c83b0e037fa63237a -->
| Pair | What changes |
|---|---|
| flat against flat fitted | Face colours and positions fitted: banners, arches and floor take their colours back |
| full against fitted full | The fitted recipe on full's finer geometry |
| lite against fitted | Same budget, moved to what the flythrough draws and fitted: arches, shadow edges and banner colours come back |
| fitted full against fitted | The same fit recipe on fewer triangles: small shapes merge or drop and edges step |
| full against lite | Fewer triangles: small shapes merge or drop and edges step; surfaces keep their colour |
| full against flat | One colour per face: a curtain's fold reads as bands |

![flat against flat fitted](../../images/render/compare-flat-flat-fitted.png)
![flat against flat fitted, the places they differ most](../../images/render/compare-flat-flat-fitted.crops.png)
![full against fitted full](../../images/render/compare-full-fitted-full.png)
![full against fitted full, the places they differ most](../../images/render/compare-full-fitted-full.crops.png)
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

<!-- generated: sponza-fidelity sha256=5670061bdcb7ee27848e61f884c4cd132470081beb4000dd2beb7ded1b677fe0 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| flat fitted | 5.468 | 15.642 | 0.7412 | 9.978 | 4.454 |
| fitted full | 5.686 | 16.220 | 0.7486 | 10.047 | 4.727 |
| fitted | 6.336 | 18.491 | 0.7060 | 11.475 | 5.208 |
| full | 9.492 | 28.107 | 0.5905 | 16.529 | 7.912 |
| lite | 11.916 | 34.234 | 0.5120 | 20.441 | 10.046 |
| flat | 13.149 | 46.465 | 0.4244 | 23.000 | 10.951 |
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

<!-- generated: sponza-flat-smooth sha256=775208847371f15579effb686080b79e0cc3bb78c60c7a1e571034567dc3ba38 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| Flat against smooth, fidelity poses | 11.109 | 38.530 | 0.4988 | 17.699 | 9.984 |
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

<!-- generated: sponza-flat-sampling sha256=fdd07d316950fab8680c9a92fd17f764c60abffe73f71385f167faa2ca00aa6c -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| fixed64 | 11.280 | 34.853 | 0.5022 | 18.828 | 9.605 |
| fixed32 | 11.305 | 34.852 | 0.5012 | 18.817 | 9.636 |
| fixed16 | 11.362 | 35.192 | 0.4958 | 18.986 | 9.663 |
| fixed8 | 11.645 | 37.238 | 0.4849 | 19.364 | 9.923 |
| area0.25 | 11.786 | 38.264 | 0.4754 | 20.130 | 9.921 |
| min4 | 11.954 | 38.779 | 0.4681 | 19.981 | 10.171 |
| fixed4 | 11.987 | 39.005 | 0.4652 | 20.005 | 10.208 |
| area0.5 | 12.197 | 41.011 | 0.4593 | 21.166 | 10.197 |
| min2 | 12.371 | 41.484 | 0.4493 | 20.906 | 10.459 |
| fixed2 | 12.625 | 43.355 | 0.4365 | 21.034 | 10.734 |
| max8 | 12.888 | 45.188 | 0.4324 | 22.362 | 10.766 |
| declared | 12.899 | 45.205 | 0.4323 | 22.369 | 10.779 |
| max32 | 12.899 | 45.205 | 0.4323 | 22.369 | 10.779 |
| sky512 | 12.912 | 45.197 | 0.4320 | 22.338 | 10.800 |
| sky64 | 12.919 | 45.187 | 0.4322 | 22.361 | 10.803 |
| sky256 | 12.922 | 45.198 | 0.4314 | 22.340 | 10.812 |
| max4 | 12.928 | 45.350 | 0.4305 | 22.374 | 10.812 |
| sky32 | 12.992 | 45.191 | 0.4320 | 22.372 | 10.888 |
| sky16 | 13.293 | 45.215 | 0.4285 | 22.390 | 11.250 |
| area2 | 13.531 | 48.041 | 0.4125 | 23.195 | 11.383 |
| centroid | 13.677 | 48.432 | 0.4037 | 23.299 | 11.544 |
| fixed1 | 13.886 | 48.293 | 0.3975 | 23.360 | 11.775 |
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

<!-- generated: sponza-indirect sha256=3a3751ab275ab7af8b1e56a963f82416734d9f5b4e232d9b3c4909bbca8ffb78 -->
| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |
|---|---:|---:|---:|---:|---:|
| full, direct light | 10.857 | 27.661 | 0.5876 | 18.506 | 9.407 |
| full, indirect light | 8.077 | 25.807 | 0.6514 | 17.154 | 6.348 |
| lite, direct light | 12.342 | 33.959 | 0.5418 | 21.856 | 10.559 |
| lite, indirect light | 10.167 | 33.220 | 0.5836 | 20.909 | 8.161 |
| flat, direct light | 12.625 | 44.871 | 0.5312 | 23.764 | 10.519 |
| flat, indirect light | 10.733 | 42.358 | 0.5364 | 22.634 | 8.467 |
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

<!-- generated: sponza-indirect-look sha256=9fea805acf22835d1b9dbe5250c9b6e44837e64513d188b039865c6defb4413d -->
| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |
|---|---:|---:|---:|---:|
| Direct light only | 10.857 | | 27.661 | 0.5876 |
| intensity 1 | 8.077 | 8.077 | 25.807 | 0.6514 |
| intensity 2 | 8.840 | 9.326 | 26.149 | 0.6353 |
| intensity 3 | 10.362 | 10.214 | 27.027 | 0.6062 |
| albedo boost 2 | 9.178 | 9.510 | 26.269 | 0.6284 |
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
