# Mesh Import

How a source mesh becomes a baked lit-mesh: the offline tools, the bake
stages and the format a renderer consumes. Drawing that mesh is
[Mesh-Rendering.md](Mesh-Rendering.md).

```mermaid
flowchart LR
    Import["Import file<br/><i>one mesh asset</i>"] --> Fetch["Fetch and check<br/>the source"]
    Fetch --> Mask["Mask alpha cards<br/><i>opt in</i>"]
    Mask --> Vis["Camera visibility<br/><i>opt in, needs a scene</i>"]
    Vis --> Thin["Thin one material<br/><i>opt in</i>"]
    Thin --> Colour{"process.light?"}
    Scene["Scene file<br/><i>adds the lights, camera<br/>region or path, tone map</i>"] -.-> Vis
    Scene -.-> Lit
    Scene -.-> Face
    Colour -- yes --> Lit["Light per vertex<br/><i>needs a scene</i>"]
    Colour -- no --> Albedo["Albedo, no light"]
    Lit --> Simp["Simplify<br/><i>opt in</i>"]
    Albedo --> Simp
    Simp --> Face["Light per face<br/><i>variants with face_samples</i>"]
    Face --> Write["Write: quantise,<br/>meshlets, octree"]
    Write --> Entry["Pack entry<br/><i>name.mesh</i>"]
    Entry --> Pack["build_pack.py<br/><i>assets.bin</i>"]
```

## Import options

Every option an import file or the fit after it has, what it changes and
what it costs. A `[process.*]` table turns its step on; without it the step
does not run. Bake times are for a source of about a quarter of a million
triangles on one desktop; frame costs were measured on the board on one mesh.

| Option | What it does | Default | Cost (bake / frame) | Section |
|---|---|---|---|---|
| `output.position_scale` | Ticks per model unit of the `int16` positions | 8 | none / none | [The baked mesh](#the-baked-mesh) |
| `materials.double_sided` | Draws these materials' faces from both sides | none | none / draws back faces too | [Import file](#import-file) |
| `process.seed` | Seeds every random ray the steps draw | 0 | none / none | [Import file](#import-file) |
| `process.alpha_mask` `keep_alpha` | Drops alpha-tested triangles that are mostly transparent | off | seconds / fewer triangles | [Import file](#import-file) |
| `process.visibility`, `source = "camera_region"`, `rounds` | Keeps what any point of the camera's region box sees | off | seconds to minutes / fewer triangles | [Import file](#import-file) |
| `process.visibility`, `source = "camera_path"`, `every_ms`, `size`, `samples`, `margin` | Keeps what any pose of the camera's path draws | off; `samples` 3, `margin` 0 | minutes / fewer triangles, about 15% less frame time on the full mesh | [Import file](#import-file) |
| `process.thin` `material`, `keep` | Keeps only a share of one material's triangles | off | none / fewer triangles | [Import file](#import-file) |
| `process.light` `ray_offset`, `colour_merge_step`, `flat_sky_rays` | Bakes the scene's sun, sky and ambient into the colours | off: albedo | minutes / none | [Import file](#import-file) |
| `process.simplify` `dense_edge`, `props`, `props_share` | Splits long edges, then simplifies each variant to its budget | off: the source's triangles | seconds / set by the budget | [Import file](#import-file) |
| `process.simplify` `seal_seams` | Joins touching pieces before simplifying | required with `simplify` | seconds / about 4% frame time | [Sealing seams](#sealing-seams) |
| `[[variants]]` `triangles` | One mesh per budget | one mesh | seconds each / set by the budget | [Import file](#import-file) |
| `[[variants]]` `face_samples` | Flat: one colour per triangle | smooth | minutes / cheaper than smooth | [The baked mesh](#the-baked-mesh) |
| `[[variants]]` `visibility` | The variant's own visibility step, in place of the import's: `process.visibility`'s keys | the import's | as `process.visibility` | [Import file](#import-file) |
| `[[variants]]` `fit` | A variant the appearance fit makes offline from the one the import bakes at `triangles`; the table is its recipe, with the SHA-256 of the recipe and of the mesh it made | not fitted | a CUDA GPU, minutes / unchanged at its budget | [Fitting a mesh to the reference](#fitting-a-mesh-to-the-reference) |

## The baked mesh

A vertex carries one sRGB colour: the baked light times the albedo, or, with no
light step, the albedo alone. A variant with `face_samples` is flat instead,
with one RGB565 colour per triangle (`light.face_colours()`): the light and
albedo averaged over the points of the face that `face_samples` sets, a fixed count
or one chosen per face, every face sharing one set
of sun and sky directions so neighbours on one surface agree unless something
really shades one of them. Vertices weld by position alone since colour no
longer splits them, and a flat mesh draws with no colour gradients. The
triangles are grouped into
**clusters**. Each cluster
owns a contiguous range of vertices and triangles, and its triangles index
only its own vertices. The clusters are the leaves of a tree rooted at
`nodes[0]`, so one box test culls a whole subtree. Positions are `int16`
ticks, `position_scale` ticks per model unit.

Smooth against flat, one pose of the same import: the sheet is the two renders
and their amplified difference, the crops are where they differ most, smooth
above flat.

![Smooth against flat](../images/render/compare-full-flat.png)
![Smooth against flat, the places they differ most](../images/render/compare-full-flat.crops.png)

`face_samples` sets how many points of a face are lit and averaged: one fixed
point against the adaptive count, the flat mesh at one pose, crops where they
differ most, fixed above adaptive. One point lights a face from one place, so
a shadow edge lands on whole faces.

![One fixed face sample against adaptive](images/import-face-samples.png)

### The pack entry

A baked mesh is an entry of type `LMSH` in the [asset pack](../assets/README.md).
Its header is 11 little-endian words; an array's offset counts from the entry's
first byte, is a multiple of 4 and is 0 for an array the mesh does not have.

| Word | Holds |
|---|---|
| 0 to 3 | vertex, triangle, cluster and node counts |
| 4 | position scale, ticks per model unit |
| 5 | positions: `int16[3]` per vertex |
| 6 | colours: `uint8[3]` per vertex, or 0 for a flat mesh |
| 7 | triangles: `uint16[3]` per triangle |
| 8 | clusters: 22 bytes each, the layout of `r3d_lit_cluster_t` |
| 9 | nodes: 16 bytes each, the layout of `r3d_lit_node_t` |
| 10 | face colours: `uint16` per triangle in the panel's RGB565, or 0 |

A mesh has vertex colours or face colours, never both. The cluster and node sizes
are checked against the C structs at compile time. `r3d_lit_mesh_from_asset()`
fills an `r3d_lit_mesh_t` with pointers into the entry, once, and nothing is
allocated or copied; the rasterizer reads that struct as it always did. Before
it does, the function checks that every array lies inside the entry and on a
4-byte boundary, every cluster's ranges lie inside the mesh, each cluster's
triangles index only its own vertices, every node's children lie inside the
cluster or node array, and an inner node's children come after it, so a walk
down the tree ends. `lit_mesh.py` writes the entry; `r3d_lit_mesh.c` is the one
reader.

## The offline tools

A mesh is an entry of the [asset pack](../assets/README.md): `<name>.mesh`, written
beside its import file by
[`launcher/tools/r3d/mesh_import.py`](../../launcher/tools/r3d/mesh_import.py)
using the offline tools in
[`launcher/tools/r3d/`](../../launcher/tools/r3d/README.md). Two kinds of file
drive it, in the manner of Unity's `.meta` beside an asset: an **import file**
describes one mesh asset, and a **scene file** describes a scenario that
places meshes ([Scene-Files.md](Scene-Files.md)). Anything specific to one mesh is in its import file; anything
about the scenario (lights, camera region, tone map) is in the scene file.
[`launcher/tools/r3d/import_settings.py`](../../launcher/tools/r3d/import_settings.py)
reads and checks both with the standard library alone, and every table is
closed: an unknown key is an error.

Run `python launcher/tools/r3d/mesh_import.py PATH` from the repository root,
with `--mesh NAME` for one mesh. `PATH` is either kind of file. An import file
with no scene-dependent step bakes alone; one with such a step refuses with
"needs a scene". A scene file bakes every mesh it places, with its own lights,
camera region and tone map. The
[scene table](Scene-Files.md#the-scene-table) is written by `scene_table.py`,
apart from the bake. `build_pack.py -o PACK` then writes the pack from the `.mesh`
entries every import and scene file names, and `rebake.py` rewrites one
`.mesh`'s clusters only.

### Import file

**Rule: an import brings the mesh in as authored, and each `[process.*]` table
present turns one processing step on.** The steps `process.light` and
`process.visibility` are scene-dependent: they read the scene's lights or
camera. A mesh without a scene-dependent step depends on no scene.

With only `[source]` and `[output]` the importer keeps the source's triangles,
cuts nothing, simplifies nothing and lights nothing. Its vertex colour is the
material's albedo (the `Kd` colour times the texture), encoded to 8 bits with a
1/2.2 gamma, not the piecewise sRGB curve, and with no tone map; the source's
own vertex colours are not read.

| Table | Fields | Meaning |
|---|---|---|
| `source` | `url`, `sha256`, `path`, `cache`, `credit` | Download, verify and locate the OBJ in its archive (a zipped OBJ at a URL is the only source kind); `credit` is the attribution line for the source model. |
| `output` | `directory`, `name`, `position_scale` | Where the scene table goes; `name` is the mesh's asset id (only without `[[variants]]`); `position_scale` overrides the format's default ticks per unit. |
| `materials` | `double_sided` | The materials whose faces are two-sided. |
| `process` | `seed` | The seed of the random rays the steps draw; allowed only with `visibility`, `thin` or `light`. |
| `process.alpha_mask` | `keep_alpha` | Drops alpha-tested triangles that are mostly transparent. |
| `process.visibility` | `source`, `rounds`; `every_ms`, `size`, `samples`, `margin` | Drops triangles the camera never sees. `source = "camera_region"`, the default, keeps what any point of the scene camera's `region` box sees in `rounds` random tries; `"camera_path"` keeps what any pose of the camera's path sees, sampled every `every_ms` at `size` pixels with `samples` squared rays a pixel, the view widened by `margin` pixels. Scene-dependent. |
| `process.thin` | `material`, `keep` | Keeps only a share of one material's triangles. |
| `process.light` | `ray_offset`, `colour_merge_step`, `flat_sky_rays` | Bakes the scene's lights into per-vertex colour. `flat_sky_rays` is the one set of sky directions the faces of a variant with `face_samples` share, and is allowed only then. Scene-dependent. |
| `process.simplify` | `dense_edge`, `props`, `props_share`, `seal_seams` | Splits long edges, then simplifies to each variant's `triangles`, reserving `props_share` of the budget for the small `props` materials; `seal_seams` joins touching pieces first. |
| `[[variants]]` | `name`, `triangles`, `face_samples`, `visibility`, `fit` | Several meshes from one import, each named. `triangles` is its budget and is required with `process.simplify`. `face_samples`, `{ fixed = N }` or `{ auto = { min, max, area } }` with `area = "median"` for the mesh median, makes the variant flat: one colour per triangle, averaged over that many points, and needs `process.light`. `visibility`, with `process.visibility`'s keys, culls this variant in place of the import's step. `fit`, with `budget`, `train_every_ms`, `held_out_every_ms`, `coverage_every_ms`, `steps`, `batch`, `laplacian`, `normal_weight`, `sha256` and `recipe_sha256`, is the recipe of a smooth variant the appearance fit makes offline ([Fitting a mesh to the reference](#fitting-a-mesh-to-the-reference)). Two variants may not produce the same mesh. |

The steps run in the order of the diagram, whatever order the file lists them.
An import without variants names its one mesh in `output.name` and cannot
simplify. A mesh's name is its asset id in the pack, at most 31 characters, and
no two meshes may share one.

Two variants of one import differ in what `simplify` keeps: the same pose
at the full budget and at about half of it, the full render above the lite.

![Full against lite](../images/render/compare-full-lite.png)
![Full against lite, the places they differ most](../images/render/compare-full-lite.crops.png)

`process.visibility` drops triangles the camera never sees, so their share
of the budget goes to what is seen. A region box keeps whatever any point in
it could see. A camera path keeps only what the path's own views show: from
every pose $v$ sampled along it, $s^2$ rays a pixel over the view widened by
$m$ pixels on each side, each starting at the near plane, and a triangle $t$
is kept if it is the first face some ray $r$ would draw:

```math
\mathrm{keep}(t) \iff \exists\, v,\ \exists\, r \in \mathrm{rays}(v, s, m):\;
t = \underset{u \,\in\, \mathrm{hits}(r),\ \mathrm{drawn}(u, r)}{\mathrm{arg\,min}}\ d_r(u)
\qquad
\mathrm{drawn}(u, r) \iff \mathrm{double}(u) \,\lor\, n_u \cdot \hat{r} < 0
```

A ray passes through a single-sided face seen from behind, as the rasterizer
culls it; when that face has a twin over the same three corners wound the
other way, the twin is what the ray sees. Faces within a small distance of
the first drawn one are kept too, since the depth test, not the ray, picks
among coincident faces. The margin and the pose spacing cover what enters
the view between two samples. `size` is one orientation's; the path is
traced through a square view as wide as its long side, which covers the
panel held either way up. The same import with the step
off, at one camera pose, crops where they differ most,
off above on: without the cull the budget is spent on hidden surfaces and
visible ones lose triangles.

![Visibility cull off against on](images/import-visibility.png)

`process.light` turns the albedo into light: the left render is the same
import with no light step, the right the baked sun, sky and ambient.

![Albedo against baked light](images/import-light.png)

`process.alpha_mask` drops the triangles of alpha-tested cards, leaves and
chains, whose texture is mostly transparent where they lie, since the
rasterizer draws no alpha test. Use it on any source with cut-out cards. Off
above on, the two poses where the meshes differ most; the step also changes
what the simplifier keeps elsewhere, so not every difference is a card.

![alpha_mask off against on](images/import-alpha-mask.png)

`process.thin` keeps a random share of one material's triangles, for a
material, such as foliage, that spends budget out of proportion to what it
shows. Off above on:

![thin off against on](images/import-thin.png)

The off/on stills in `images/` are not made by the doc-images workflow: each
"off" side is a scratch bake of the import with that step's table removed,
which needs the bake toolchain and the source model, so nothing refreshes them
when the bake changes.

The scene file that places meshes and carries the lights, the camera and the
tone map is described in [Scene-Files.md](Scene-Files.md).

Each tool and its module, `rebake.py` and when to rebake rather than bake in
full, and the triangle-size report are in
[`launcher/tools/r3d/README.md`](../../launcher/tools/r3d/README.md).

## Fidelity against a reference

How close is a bake to the ground truth, and where is it off? The reference
renderer lights the full source mesh, per pixel, with the scene's own lights,
shadows and tone map, at the device render size and supersampled, for the
poses of the scene's camera path. A host render of any variant, smooth, lite
or flat, is scored against it:

| Number | Meaning |
|---|---|
| Mean ΔE76 | CIE76 colour difference per pixel, averaged; about 2 is just visible, 20 is a clearly different colour |
| p95 ΔE76 | The 95th percentile of the pixels' ΔE, averaged over frames: how bad the worst places are |
| Luma SSIM | Structural similarity over 8x8 windows of luma, 1 for identical: whether shapes and contrast match |
| Edge and interior ΔE76 | The same ΔE on pixels at a sharp luma step of the reference (a silhouette, a lit or shadowed boundary) and on all others |

Heatmaps put each pixel's ΔE on a scale from black (a match) through red
(about 20) to yellow (50 or more). The sheet beside them shows the reference,
the render, the heatmap and the edge pixels, and the commands that make all of
it are in [`launcher/tools/r3d/README.md`](../../launcher/tools/r3d/README.md#fidelity-reference).
A scene's scores and example sheet live beside its own tools. Nothing
refreshes them when the bake changes: the reference and the scratch bakes need
the bake toolchain and the source model.

The ceiling for a flat bake is the smooth bake's own error. Smooth, one colour
per vertex, is not exact either, and flat adds the colour gradient across each
triangle, which no face sampling brings back. What face sampling does change is
how well the one colour represents the face.

### The scores, exactly

A pixel's 8-bit colour $c$ is decoded with the display gamma $\gamma = 2.2$,
taken to CIE XYZ through the linear sRGB primaries, and to CIELAB relative to
the D65 white. The constants live in `render_compare.py`, which both the
scoring and the fit's loss read.

```math
\ell = \left(\frac{c}{255}\right)^{\gamma},
\qquad
\begin{pmatrix}X\\Y\\Z\end{pmatrix} =
\begin{pmatrix}
0.4124564 & 0.3575761 & 0.1804375\\
0.2126729 & 0.7151522 & 0.0721750\\
0.0193339 & 0.1191920 & 0.9503041
\end{pmatrix}\ell
```

```math
f(t) = \begin{cases} t^{1/3} & t > \delta^3\\[2pt] \dfrac{t}{3\delta^2} + \dfrac{4}{29} & \text{otherwise}\end{cases},
\qquad \delta = \frac{6}{29},
\qquad (X_n, Y_n, Z_n) = (0.95047,\ 1,\ 1.08883)
```

```math
L^* = 116\,f\!\left(\tfrac{Y}{Y_n}\right) - 16,
\qquad a^* = 500\left(f\!\left(\tfrac{X}{X_n}\right) - f\!\left(\tfrac{Y}{Y_n}\right)\right),
\qquad b^* = 200\left(f\!\left(\tfrac{Y}{Y_n}\right) - f\!\left(\tfrac{Z}{Z_n}\right)\right)
```

```math
\Delta E_{76}(p) = \left\lVert \mathrm{Lab}(R_p) - \mathrm{Lab}(T_p) \right\rVert_2
```

for render $R$ and reference $T$ at pixel $p$. Mean ΔE averages
$\Delta E_{76}(p)$ over the frame's pixels and p95 is its 95th percentile.
Luma SSIM works on the gamma-encoded luma $y = 0.2126 r + 0.7152 g + 0.0722 b$
(channels 0 to 1), over every 8 by 8 window $w$ of the frame, with the
window's means $\mu$, variances $\sigma^2$ and covariance $\sigma_{RT}$:

```math
\mathrm{SSIM} = \frac{1}{|W|}\sum_{w \in W}
\frac{(2\mu_R\mu_T + C_1)(2\sigma_{RT} + C_2)}{(\mu_R^2 + \mu_T^2 + C_1)(\sigma_R^2 + \sigma_T^2 + C_2)},
\qquad C_1 = 0.01^2,\quad C_2 = 0.03^2
```

The edge pixels are those within one pixel of a step in the reference's luma
steeper than 0.06 a pixel; edge ΔE averages $\Delta E_{76}$ over them and
interior ΔE over the rest:

```math
E = \mathrm{dilate}_1\left\{\, p : \left\lVert \nabla y_T(p) \right\rVert > 0.06 \,\right\}
```

## Sweeping the flat bake

`bake_fidelity.py` re-lights a flat mesh's simplified geometry with chosen
settings into a scratch directory, builds the host renderer with that mesh in
place of the tracked one, and scores it, so a setting is judged by its distance
to the reference and nothing tracked changes. Its default variant is the bake
the import file declares, byte for byte. A sweep over a scene found:

- **Samples per face** are the lever. The score converges at about 16 fixed
  samples; more adds nothing. Raising the minimum helps more than raising the
  maximum or shrinking the area, because most faces are small and the adaptive
  count gives them one sample.
- **Sky rays** are saturated at the bake's default; fewer is worse and more is
  noise.
- **Centroid placement** loses to the stratified points, and takes every count
  to the same colours. **A centre-only sun** loses too: it makes every shadow
  edge hard, where the disc blends it.
- **Edge error is structural.** Samples lower the error at lit and shadow
  edges, but it stays more than twice the interior error, and the smooth bake's
  own edge error is close to the best flat one. A face holds one colour, so a
  boundary through it cannot be sampled away, and decimation misplaces
  silhouettes before lighting.

## Fitting a mesh to the reference

The simplifier keeps what it can of the source's colour and shape, but it
never looks at an image. `appearance_simplify.py` does: it takes a smooth
bake at its triangle budget, draws it with a differentiable rasterizer the
way the device draws it, and moves the vertices and changes their colours
until the renders match the reference over the camera path's poses. The
triangles stay as they were, so the budget and the frame cost hold.

A variant with a `fit` table records the recipe: the budget it prunes to,
the poses it trains on, holds out and counts pixels over, its optimiser
settings, and the SHA-256 of the mesh it made. The bake does not run the fit,
which needs a GPU: it checks that the committed mesh is the one the recipe
records. `fitted_variant.py` remakes it, in two steps, one per environment.

**When to use it:** a mesh seen along a known set of views, at a budget
where the simplifier's colours and silhouettes visibly drift from the
source. **What it costs:** a CUDA GPU and minutes per mesh, nothing at run
time. A scene's crop sheets for every stage, before above after, live in that
scene's tools README beside its scores.

```mermaid
flowchart LR
    S[simplified smooth bake] --> F[fit positions and colours]
    P[camera path poses] --> R[reference renders]
    R --> F
    F --> W[write_lit_mesh]
    W --> H[host render, held-out poses]
    H --> C[render_compare.py score, sheets, heatmaps]
```

The fitted colours are a bake in their own right, so the fitted mesh enters
the import at its last stage, the writer, and is never lit again. One mesh
fits the whole path, or one mesh fits each segment of it, to be swapped as
the camera moves; segments see fewer poses each and fit poses between them
less well. A fit is judged on poses it never trained on, by the same scores
and pictures as any other bake: a sheet and enlarged crops against the
simplifier's mesh and against the reference, and the heatmap sheet. A
scene's example lives beside its own tools; nothing refreshes it, since the
fit needs a CUDA GPU and its own environment
([`launcher/tools/r3d/README.md`](../../launcher/tools/r3d/README.md#appearance-fit)).

### The fit's objective

The fit draws the mesh with a differentiable rasterizer $\mathcal{R}$
(nvdiffrast) as the device does, and moves the welded positions $P$ and
vertex colours $C$ to minimise, over a random batch $B$ of training poses
each step, the mean ΔE above against the reference $T_v$ of pose $v$, plus
a regulariser that keeps the mesh's local shape:

```math
\min_{P,\,C}\;
\frac{1}{|B|}\sum_{v \in B}\frac{1}{|\Omega|}\sum_{p \in \Omega}
\Delta E_{76}\!\left(\mathcal{R}_v(P, C)_p,\; T_{v,p}\right)
\;+\;
\lambda\,\frac{1}{|V|}\sum_{i \in V}
\left\lVert \frac{\mathcal{L}(P)_i - \mathcal{L}(P^0)_i}{\bar{e}} \right\rVert^2
```

```math
\mathcal{L}(P)_i = P_i - \frac{1}{|N(i)|}\sum_{j \in N(i)} P_j
```

$\Omega$ is the frame's pixels, $P^0$ the start positions, $N(i)$ the
positions sharing an edge with $i$, $\bar{e}$ the start's mean edge length
and $\lambda$ the `--laplacian` weight. Adam takes the steps, both learning
rates decay as $\eta_k = \eta_0 \cdot 0.1^{k/K}$ over $K$ steps, and the
colours are clamped to $[0, 1]$ after each. Where nothing is drawn the
renderer shows the scene's clear colour, as the device and the reference do.
$`\mathcal{E}_{\Delta E}`$ below names the first term and
$`\mathcal{E}_{\mathcal{L}}`$ the second.

## Spending the budget where it shows and costs least

The fit can also choose where the triangles go and weigh what they cost.
Each stage is optional and runs in this order:

```mermaid
flowchart LR
    V[path visibility<br/>on the source] --> S[simplify to more<br/>than the budget]
    S --> P[prune to the budget]
    P --> R[refine a coarse fit<br/>optional]
    R --> F[fit: ΔE, Laplacian,<br/>normals, cost]
    F --> W[write_lit_mesh]
```

**Path visibility** is the import's `camera_path` source above. *What:* only
surfaces some pose draws get budget. *When:* a mesh seen from a known path.
*Cost:* minutes of ray casting per import; it cuts a mesh's triangles, not its
pixels, so a culled mesh looks the same and draws faster.

**Pruning.** *What:* every pose of a dense pose set draws the mesh and counts
the pixels $a_t$ each triangle shows; triangles with $a_t = 0$ go first, then
those with the smallest $a_t$, down to the budget. Simplifying to more than
the budget and pruning back puts the triangles where a pose shows them.
*When:* always with a path. *Cost:* seconds.

**Warm start.** *What:* a fitted coarse mesh gets its worst triangles, by ΔE
summed over the pixels they show, split along their longest edge, both sides
at once, up to a larger budget, and is fitted again. *When:* to grow a fit
instead of starting a finer one from the simplifier. *Cost:* one more fit.

**The normal term.** *What:* colour alone can be matched by geometry that is
wrong and shows it from another view. The reference renderer also writes the
source's shading normal per pixel, turned toward the eye, and the fit draws
its own: area-weighted vertex normals $\hat{n}$, interpolated and turned the
same way. $\Omega_v^{\cap}$ is the pixels both cover, so coverage stays the
colour term's business, through the scene's clear colour. *When:* always; it
leaves ΔE where it was and brings the normals back toward the source. *Cost:*
a second drawing per view. The error reported beside ΔE is the mean angle:

```math
\theta = \frac{1}{|\Omega^{\cap}|}\sum_{p \in \Omega^{\cap}} \arccos\!\left(\hat{n}_p \cdot n^{\mathrm{ref}}_p\right)
```

**The cost model.** A frame's time from pose $v$ is linear in what the
renderer does: a constant, the triangles of the clusters in view $N_{s,v}$
(fetched, transformed and tested), the drawn triangles $D_v$ (in front of the
eye, facing it or double-sided, on screen), their screen rows $\rho_t$, the
pixels they cover before the depth test $\alpha_t$ (overdraw counted) and the
clusters in view $N_{c,v}$:

```math
\hat{t}_v = w_0 + w_s\,N_{s,v} + w_d\,|D_v| + w_\rho \sum_{t \in D_v} \rho_t + w_\alpha \sum_{t \in D_v} \alpha_t + w_c\,N_{c,v}
```

The weights are non-negative least squares over board frame times of meshes
with different triangle counts and overdraw, at the poses the board times.
`cost_model.py` fits and applies them, and keeps them, with the frames they
were fitted to, in a weights file beside it.

**The cost term.** *What:* $D_v$, $\rho_t$ and $\alpha_t$ follow the vertex
positions, so the fit can trade appearance against predicted time with a
weight $\kappa$ in ΔE per millisecond. *When:* when a smaller budget is not an
option; on the meshes it was tried on, a smaller budget bought the same time
for less error. *Cost:* the fit runs about three times longer.

The whole objective, with $\lambda$, $\lambda_n$ and $\kappa$ the weights of
the Laplacian, normal and cost terms:

```math
\min_{P,\,C}\; \mathcal{E}_{\Delta E}(P, C) + \lambda\,\mathcal{E}_{\mathcal{L}}(P)
+ \lambda_n\,\frac{1}{|B|}\sum_{v \in B}\frac{1}{|\Omega_v^{\cap}|}\sum_{p \in \Omega_v^{\cap}}\left\lVert \hat{n}_v(P)_p - n^{\mathrm{ref}}_{v,p} \right\rVert_1
+ \kappa\,\frac{1}{|B|}\sum_{v \in B}\hat{t}_v(P)
```

Sweeping the budget and $\kappa$ gives held-out ΔE against predicted
milliseconds; the meshes no other is better than on both form the Pareto
front, and its knee is where more triangles stop buying visible error.

## Sealing seams

Simplifying a model made of many separate pieces approximates each piece's
border on its own, and a border that erodes leaves a pixel-sized empty spot
where another surface should meet it. `seal_seams = true` in
`[process.simplify]`, where the key is required, imports the same mesh
differently; `false` simplifies the pieces as they are. It acts in the simplifier's stage
only; the bake after it is unchanged.

```mermaid
flowchart LR
    load[Load and light] --> join[Join touching pieces]
    join --> pass[Simplify pass]
    pass --> merge[Merge near colours]
    merge --> bake[Quantise, meshlets, octree]
    subgraph seal_seams
        join
        pass
        merge
    end
```

| Step | What it does |
|---|---|
| Join | Welds border vertices within one quantisation step and splits border edges at the vertices lying on them, so a shared edge is one edge. |
| Pass | Runs meshoptimizer with light regularizing instead of regularizing. |
| Merge | Gives vertices at one quantised position one colour when they differ by at most one and a half RGB565 steps. |

It cuts the empty spots but does not remove them. The cost is about 4% frame
time (about 3% more triangles drawn) on the mesh it was measured on.

The same import with `seal_seams` off against on, at a pose where it shows,
off above on: a pixel-sized hole that shows the sky is sealed.

![seal_seams off against on](images/import-seal-seams.png)

## Meshlets

The clusters are **meshlets**: compact runs of at most 32 triangles from
meshoptimizer's clusterizer, each of one sidedness and owning the vertices its
triangles use. The octree above them is built over the meshlets' centres, and
its leaves hold a few hundred triangles' worth.

Meshlets share more vertices than clusters cut as leaves of an octree of the
triangles, so a frame transforms fewer. Bigger ones span looser boxes and
submit more triangles for the same view, and smaller ones cost more clusters
to walk. The size trades these: 64 lost on the board and 32 won.

Meshlets also change the draw order of the finest level. Where two triangles
reach the same depth the first drawn wins, so a redrawn frame differs from the
old clustering's in a fraction of a percent of its pixels, from ties alone:
the triangles are the same.

Levels of detail built on the meshlets, and what they would save, are in
[plans/Cluster-LOD.md](../plans/Cluster-LOD.md).
