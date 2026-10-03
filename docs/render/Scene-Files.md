# Scene Files

A scene file places objects. Every object has exactly one component: a mesh
renderer, directional light, or camera. A mesh's source, geometry processing,
and variants belong in its [import file](Mesh-Import.md#import-file); the scene
supplies the conditions that make a placed renderer's baked output.

## Objects

Every object has a unique `name`, an optional transform and exactly one
component table. `position`, `rotation`, and `scale` default to the origin,
zero rotation, and unit scale. A zero light rotation points its sun straight
overhead.

| Component | Fields | What it is |
|---|---|---|
| `mesh_renderer` | `mesh`, `variant`, `bake`, `shading`, `visibility`, `fit`, `indirect` | Draws a mesh asset. `mesh` names an import file beside the scene; `variant` chooses its geometry variant. |
| `light` | `type`, `color`, `intensity`, `disc_degrees`, `rays` | A directional light. Its direction toward the light is the object's +Y axis turned by its rotation. Position and scale do not matter. |
| `camera` | `half_fov_short_tan`, `near_z`, `region`, `path`, `background` | The view. `path` names tracks from `tools/anim/bake_tracks.py` and their glTF node; it writes `<tracks>_tracks_generated.c` and `.h` beside the scene. `background` is what uncovered pixels show, on the panel and in the reference. |

An appearance-fit recipe is grouped below its renderer's `fit` table.

| Group | Key | Meaning |
|---|---|---|
| `fit.prune` | `budget`, `coverage_every_ms` | Triangle budget and camera-path sampling interval for pruning. |
| `fit.poses` | `train_every_ms`, `held_out_every_ms` | Camera-path training poses and the multiples held out for scoring. |
| `fit.optimise` | `steps`, `batch`, `laplacian`, `normal_weight` | Optimiser iteration count, batch size and loss weights. |
| `fit.hashes` | `sha256`, `recipe_sha256` | Hashes of the fitted mesh and its effective recipe. |

```toml
[objects.mesh_renderer.fit.prune]
budget = 8672
coverage_every_ms = 100

[objects.mesh_renderer.fit.poses]
train_every_ms = 1000
held_out_every_ms = 5000

[objects.mesh_renderer.fit.optimise]
steps = 2000
batch = 8
laplacian = 10.0
normal_weight = 1.0

[objects.mesh_renderer.fit.hashes]
sha256 = "..."
recipe_sha256 = "..."
```

A double-sided face turns toward the lights.

## Option reference

Keys before `;` are required; after it, optional. The import option reference
is in [Mesh-Import.md](Mesh-Import.md#import-options).

### Scene settings

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `[bake]` | `ray_offset`, `colour_merge_step`; `flat_sky_rays`, `indirect = { bounces, rays, cache_samples }` | Scene-wide tracing settings. | Required by a baked renderer. | [bake](#bake) |
| `[indirect]` | `; intensity, albedo_boost` | Sets the appearance of the baked bounce-light cache. | `1.0 each`; rejected unless a baked renderer uses [bake].indirect. | [indirect](#indirect) |
| `[sky]` | `color`, `intensity`, `rays` | Hemisphere light for bakes. | Read only by a baked renderer. | [sky](#sky) |
| `[ambient]` | `color`, `intensity` | Constant light for bakes. | Read only by a baked renderer. | [ambient](#ambient) |
| `tonemap_white` | value | Tone-map white point. | Read only by a baked renderer. | [tonemap_white](#tonemap_white) |

#### bake

`ray_offset` starts a tracing ray clear of its source surface and
`colour_merge_step` controls colour merging after the bake. `flat_sky_rays` is
required for [flat shading](#shading-flat). The image compares the renderer's
`bake = true` result with albedo.

![Albedo against baked light](images/import-light.png)

#### bake: indirect

`[bake].indirect = { bounces = K, rays = R, cache_samples = S }` enables the
scene-wide bounce-light cache. All fields are required when it is present.

| Field | Meaning |
|---|---|
| `bounces` | How many times light bounces; 0 turns bounce gathering off. |
| `rays` | Cosine-weighted hemisphere rays for each gather. |
| `cache_samples` | Points averaged into each source triangle's direct radiance. |

`rays` and `cache_samples` are at least 1 and `bounces` is at least 0. The
reference renderer reads the same settings. A renderer can opt out with
[indirect: off](#indirect-off).

#### indirect

`intensity` is at least 0 and multiplies gathered bounce light.
`albedo_boost` is greater than 0 and replaces bounce reflectance with
$`\min(\beta a, \max(a, 0.99))`$. The bake stores the result in the same vertex
or face colours as direct light.

![The reference beside the direct-light and bounce bakes, each with its error heatmap](../images/render/bake-indirect-compare.png)

#### sky

`[sky]` supplies coloured hemisphere light; its `rays` are random hemisphere
directions used by the bake.

#### ambient

`[ambient]` adds its constant coloured light everywhere in the bake.

#### tonemap_white

`tonemap_white` controls the tone map applied to baked light.

### mesh_renderer

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `bake: renderer` | `true` | Traces this renderer against its own source. | `false`. | [bake renderer](#bake-renderer) |
| `variant` | name | Chooses a named geometry variant. | Required when the import has variants. | [variant](#variant) |
| `visibility: camera_region` | `source = "camera_region"`, `rounds` | Keeps triangles visible from any point in the camera's region. | Off. | [visibility: camera_region](#visibility-camera_region) |
| `visibility: camera_path` | `source = "camera_path"`, `every_ms`, `size`; `samples`, `margin` | Keeps triangles first seen from sampled camera-path views. | Off. | [visibility: camera_path](#visibility-camera_path) |
| `shading: smooth` | `"smooth"` | Stores baked colour at vertices. | `"smooth"`. | [shading: smooth](#shading-smooth) |
| `shading: flat` | `flat = { fixed = N }` / `flat = { auto = { min, max, area } }` | Stores one averaged RGB565 colour per face. | Off. | [shading: flat](#shading-flat) |
| `fit: recipe` | `budget`, `train_every_ms`, `held_out_every_ms`, `coverage_every_ms`, `steps`, `batch`, `laplacian`, `normal_weight`, `sha256`, `recipe_sha256` | Records the appearance-fit recipe. | Off. | [fit: recipe](#fit-recipe) |
| `fit: normal weight` | `normal_weight` | Weighs normal agreement in the fit objective. | `None`; required in `fit`. | [fit: normal weight](#fit-normal-weight) |
| `fit: budget and cost` | recipe budget, coverage, and fit weights | Selects and weighs visible geometry. | Off. | [fit: budget and cost](#fit-budget-and-cost) |
| `indirect: off` | `indirect = false` | Bakes this renderer without bounce light. | Bounce light on when the scene recipe is present. | [indirect: off](#indirect-off) |

#### bake renderer

`bake = true` writes this renderer's lit mesh; otherwise it draws the shared
imported albedo mesh.

#### variant

`variant` selects a named geometry output from its import.

#### visibility: camera_region

The region source casts from points in the camera's `region`. Use it when the
camera may occupy the box without a defined path. The same import with the
cull off, crops where they differ most, is off above on: without it the budget
goes to hidden surfaces.

![Triangles the camera path never sees](images/import-visibility.png)

#### visibility: camera_path

The path source retains a triangle when a sampled camera view can draw it.
Poses $v$ are sampled along the path; $s^2$ rays are a pixel apart across the
view widened by $m$ pixels on each side from the near plane, and a triangle is
kept if the first face some ray would draw is it. Here $`d_r(u)`$ is ray $r$'s
distance to face $u$, `double(u)` says that face is double-sided, and
$`\hat r`$ is the ray direction:

```math
\mathrm{keep}(t) \iff \exists\, v,\ \exists\, r \in \mathrm{rays}(v, s, m):\;
t = \underset{u \in \mathrm{hits}(r),\ \mathrm{drawn}(u, r)}{\mathrm{arg\,min}}\ d_r(u)
\qquad
\mathrm{drawn}(u, r) \iff \mathrm{double}(u) \,\lor\, n_u \cdot \hat{r} < 0
```

A single-sided face seen from behind does not stop a ray; its twin wound the
other way is what the ray sees. Faces within a small distance of the first
drawn face are kept too, since the depth test picks among coincident faces.

![Triangles the camera path never sees](images/appearance-path-culled.png)
![Culled lite against uncut, largest differences](images/appearance-path-culled.crops.png)

#### shading: smooth

Smooth shading bakes one colour per vertex. It is the normal baked mesh form
described in [The baked mesh](Mesh-Import.md#the-baked-mesh).

#### shading: flat

Flat shading stores one RGB565 colour per triangle. `fixed` uses that many
lighting points per face; `auto` chooses a count from face area. Smooth is
above flat; fixed is above adaptive. One point lights a face from one place, so
a shadow edge lands on whole faces.

![Smooth against flat](../images/render/compare-full-flat.png)
![Smooth against flat, the places they differ most](../images/render/compare-full-flat.crops.png)
![One fixed face sample against adaptive](images/import-face-samples.png)

Face samples are the useful control; extra samples converge, and more sky rays
add noise after the bake setting is saturated. Stratified placement and a disc
sun preserve soft boundaries. A face's one colour leaves edge error that
sampling cannot remove. Generated findings and sheets live in the scene tools
README.

#### fit: recipe

The appearance fit starts from a smooth bake at its budget and adjusts welded
positions and vertex colours against reference renders from camera-path poses.
It records its complete recipe and output hashes; `fitted_variant.py` remakes
it in its GPU environment, while a bake checks the recorded mesh. The reference,
fitted-output, crop and heatmap sheets live in the scene tools README.

![Simplifier against the reference](../images/render/appearance-lite-reference.png)
![Fitted mesh against the reference](../images/render/appearance-chosen-heat.png)
![Simplifier and fitted mesh, largest differences](../images/render/appearance-lite-fitted-reference.crops.png)

The fit draws welded positions $P$ and vertex colours $C$ over batch $B$ of
poses, with frame pixels $\Omega$, start positions $P^0$, edge neighbours
$N(i)$, start mean edge length $\bar e$, fitted normal $\hat n$, reference
normal $n^{\rm ref}$, and predicted frame time $`\hat t_v`$:

```math
\min_{P,\,C}\; \frac{1}{|B|}\sum_{v \in B}\frac{1}{|\Omega|}\sum_{p \in \Omega}
\Delta E_{76}\!\left(\mathcal{R}_v(P, C)_p,\; T_{v,p}\right)
+ \lambda\,\frac{1}{|V|}\sum_{i \in V}\left\lVert \frac{\mathcal{L}(P)_i - \mathcal{L}(P^0)_i}{\bar e} \right\rVert^2
+ \lambda_n\,\frac{1}{|B|}\sum_{v \in B}\frac{1}{|\Omega_v^{\cap}|}\sum_{p \in \Omega_v^{\cap}}\left\lVert \hat n_v(P)_p - n^{\rm ref}_{v,p} \right\rVert_1
+ \kappa\,\frac{1}{|B|}\sum_{v \in B}\hat t_v(P)
```

```math
\mathcal{L}(P)_i = P_i - \frac{1}{|N(i)|}\sum_{j \in N(i)} P_j
```

#### fit: normal weight

Colour alone permits geometry that matches one view but differs from another.
`normal_weight` adds mean L1 fitted/reference normal difference where both
cover a pixel. The reference and fit turn normals toward the eye. The generated
normal-angle heatmaps and sweep findings live in the scene tools README.

![Normal angle heatmaps](images/appearance-normal-heat.png)

#### fit: budget and cost

The stages are path visibility, simplify above the target, prune to budget,
optional warm start, then fitting. [Path visibility](#visibility-camera_path)
spends budget only on surfaces the path draws. Pruning removes triangles with
the least covered pixels. A warm start splits worst fitted triangles and
refines the larger mesh. The cost model learns non-negative weights from board
frame times; its cost term can trade appearance for predicted time, though a
smaller budget can be the better trade. Tables and generated findings live in
the scene tools README.

#### indirect: off

`indirect = false` requires `[bake].indirect` and omits its bounces from this
renderer and its fit reference.

### Light options

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `light` | `type`, `color`, `intensity`, `disc_degrees`, `rays` | Directional bake light. | Required for an object with this component. | [light](#light) |

#### light

Only directional lights are scene objects; `point` and `spot` are reserved.

### Camera options

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `camera` | `half_fov_short_tan`, `near_z`; `region`, `path`, `background` | Defines the scene view. | One camera at most. | [camera](#camera) |

#### camera

Without a path the camera sits at its transform looking down -Z.

## What the scene must carry

A renderer with `bake = true` reads lights, `[sky]`, `[ambient]`,
`tonemap_white`, `[bake]`, and its `[indirect]` look. Its `visibility` reads
the camera `region`, or `path` for the path source.

## The scene table

`scene_table.py` writes `<scene>_scene_generated.c` and `.h` beside the scene
file. The table is `<scene>_scene`, registered with `SCENE_REGISTER()`. Its
header names objects `<SCENE>_SCENE_<OBJECT>`, so a misspelt object fails to
compile. An asset id missing from the pack fails `scene_load()`. The transform
is baked to a 3x3 plus a position, so the device does no trigonometry.
