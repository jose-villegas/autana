# Scene Files

A scene file places objects. Every object has a transform and exactly one
component: a mesh renderer, directional light, or camera. A mesh's source,
geometry processing, and variants belong in its [import file](Mesh-Import.md#import-file);
the scene supplies the conditions that make a placed renderer's baked output.

## A minimal scene

```toml
tonemap_white = 0.35             # larger is darker

[sky]                            # scene settings, not objects
color = [0.55, 0.68, 0.9]
intensity = 0.9
rays = 48

[bake]
ray_offset = 0.5
colour_merge_step = 6

[[objects]]
name = "sun"
rotation = [18.4, -48.7, 0.0]    # pitch, yaw, roll in degrees

[objects.light]
type = "directional"
color = [1.0, 0.92, 0.78]
intensity = 3.0
disc_degrees = 1.2
rays = 8

[[objects]]
name = "camera"

[objects.camera]
half_fov_short_tan = 0.62
near_z = 6.0
region = { min = [-1400.0, 20.0, -620.0], max = [1270.0, 1250.0, 550.0] }

[[objects]]
name = "hall"

[objects.mesh_renderer]
mesh = "hall.import.toml"       # import file beside this scene
bake = true                      # write this renderer's lit mesh
visibility = { source = "camera_region", rounds = 160 }
```

The light, sky, tone map and `[bake]` settings are read only while baking.
The camera and renderer become entries in the [scene table](#the-scene-table).

## Objects

Every object has a unique `name`, an optional transform and exactly one
component table.

| Key | Meaning |
|---|---|
| `position` | Where the object is, in model units. Default `[0, 0, 0]`. |
| `rotation` | `[pitch, yaw, roll]` in degrees, right-handed: roll about z, then pitch about x, then yaw about y. Default `[0, 0, 0]`. |
| `scale` | Per-axis scale, positive on every axis. Default `[1, 1, 1]`. |

| Component | Fields | What it is |
|---|---|---|
| `mesh_renderer` | `mesh`, `variant`, `bake`, `shading`, `visibility`, `fit`, `indirect` | Draws a mesh asset. `mesh` names an import file beside the scene; `variant` chooses its geometry variant. `bake = true` traces this renderer against its own source using this scene's settings. Without it the shared imported albedo mesh is drawn. |
| `light` | `type`, `color`, `intensity`, `disc_degrees`, `rays` | A directional light. The direction toward it is the object's +Y axis turned by its rotation. Position and scale do not matter. `point` and `spot` are reserved. |
| `camera` | `half_fov_short_tan`, `near_z`, `region`, `path`, `background` | The view. `region` is the box the camera moves within; `path = { tracks, node }` names baked animation tracks and their glTF node. `background` is 0xRRGGBB, default black. Without a path the camera sits at its transform, looking down its -Z. A scene has at most one camera. |

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

### Baking

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `[bake]` | `ray_offset`, `colour_merge_step`; `flat_sky_rays`, `indirect = { bounces, rays, cache_samples }` | Scene-wide tracing settings. | Required by a baked renderer. | [bake](#bake) |

#### bake

`ray_offset` starts a tracing ray clear of its source surface and
`colour_merge_step` controls colour merging after the bake. `flat_sky_rays` is
required when a renderer uses [flat shading](#shading-flat). The optional
`indirect` recipe enables the scene-wide bounce-light cache.

![Albedo against baked light](images/import-light.png)

### Visibility

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `visibility: camera_region` | `source = "camera_region"`, `rounds` | Keeps triangles visible from any point in the camera's region. | Off. | [visibility: camera_region](#visibility-camera_region) |
| `visibility: camera_path` | `source = "camera_path"`, `every_ms`, `size`; `samples`, `margin` | Keeps triangles first seen from sampled camera-path views. | Off. | [visibility: camera_path](#visibility-camera_path) |

#### visibility: camera_region

The region source casts from points in the camera's `region`. Use it when the
camera may occupy the box without a defined path.

#### visibility: camera_path

The path source retains a triangle when a sampled camera view can draw it. A
sampled pose casts a grid of rays from its near plane across the widened view:

```math
\mathrm{keep}(t) \iff \exists\, v,\ \exists\, r \in \mathrm{rays}(v, s, m):\;
t = \underset{u \,\in\, \mathrm{hits}(r),\ \mathrm{drawn}(u, r)}{\mathrm{arg\,min}}\ d_r(u)
\qquad
\mathrm{drawn}(u, r) \iff \mathrm{double}(u) \,\lor\, n_u \cdot \hat{r} < 0
```

Single-sided back faces do not stop a ray, matching rasterizer culling. The
margin and pose spacing cover geometry entering between samples. `size` is one
orientation; tracing a square as wide as the longer panel side covers both.
The generated comparison sheet and crops show the result and the rejected
triangles.

![Triangles the camera path never sees](images/appearance-path-culled.png)
![Culled lite against uncut, largest differences](images/appearance-path-culled.crops.png)

### Shading

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `shading: smooth` | `"smooth"` | Stores baked colour at vertices. | `"smooth"`. | [shading: smooth](#shading-smooth) |
| `shading: flat` | `flat = { fixed = N }` / `flat = { auto = { min, max, area } }` | Stores one averaged RGB565 colour per face. | Off. | [shading: flat](#shading-flat) |

#### shading: smooth

Smooth shading bakes one colour per vertex. It is the normal baked mesh form
described in [The baked mesh](Mesh-Import.md#the-baked-mesh).

#### shading: flat

Flat shading stores one RGB565 colour per triangle. `fixed` uses that many
lighting points per face; `auto` chooses a count from face area. The comparison
sheet and crops show the smooth/flat and fixed/adaptive differences.

![Smooth against flat](../images/render/compare-full-flat.png)
![Smooth against flat, the places they differ most](../images/render/compare-full-flat.crops.png)
![One fixed face sample against adaptive](images/import-face-samples.png)

### Fit

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `fit: recipe` | `budget`, pose spacing, optimiser settings, hashes | Records the smooth appearance-fit recipe for this renderer. | Off. | [fit: recipe](#fit-recipe) |
| `fit: normal weight` | `normal_weight` | Weighs normal agreement in the fit objective. | Required by `fit`. | [fit: normal weight](#fit-normal-weight) |

#### fit: recipe

The appearance fit starts from a smooth bake at its budget and adjusts welded
positions and vertex colours against reference renders from the camera path.
It records the budget, pose schedule, optimiser settings, and output hashes.
The bake checks the recorded mesh; `fitted_variant.py` remakes it in its GPU
environment. The reference, fitted-output and crop sheets show its result.

![Simplifier against the reference](../images/render/appearance-lite-reference.png)
![Fitted mesh against the reference](../images/render/appearance-chosen-heat.png)
![Simplifier and fitted mesh, largest differences](../images/render/appearance-lite-fitted-reference.crops.png)

#### fit: normal weight

Colour alone permits geometry that matches one view but differs from another.
`normal_weight` adds the mean L1 difference between the fitted and reference
normal buffers where both cover a pixel. The generated normal-angle heatmaps
show the effect; scores and sweep results live with the scene tools.

![Normal angle heatmaps](images/appearance-normal-heat.png)

The fit minimizes the image term, a Laplacian shape term, the normal term and
an optional predicted-cost term:

```math
\min_{P,\,C}\; \mathcal{E}_{\Delta E}(P, C) + \lambda\,\mathcal{E}_{\mathcal{L}}(P)
+ \lambda_n\,\frac{1}{|B|}\sum_{v \in B}\frac{1}{|\Omega_v^{\cap}|}\sum_{p \in \Omega_v^{\cap}}\left\lVert \hat{n}_v(P)_p - n^{\mathrm{ref}}_{v,p} \right\rVert_1
+ \kappa\,\frac{1}{|B|}\sum_{v \in B}\hat{t}_v(P)
```

### Indirect look

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `[indirect]` | `intensity`, `albedo_boost` | Sets the appearance of the baked bounce-light cache. | Required when `[bake].indirect` is present. | [indirect](#indirect) |

#### indirect

`intensity` multiplies gathered bounce light. `albedo_boost` changes the
reflectance used at each bounce while keeping it below one. The reference uses
the same settings. The bake recipe is in [bake](#bake); it stores the result in
the same vertex or face colours as direct light.

![The reference beside the direct-light and bounce bakes, each with its error heatmap](../images/render/bake-indirect-compare.png)

## What the scene must carry

A renderer with `bake = true` reads lights, `[sky]`, `[ambient]`,
`tonemap_white`, `[bake]`, and its `[indirect]` look. Its `visibility` reads
the camera `region`, or `path` for the path source. A scene may carry these
only when a baked renderer reads them.

A baked output is `<scene>.<object>.mesh` beside the scene file, so one import
can have independent bakes in different scenes. An albedo renderer uses its
import's `<variant>.mesh`. A baked renderer is baked where it sits, so its
object transform must be identity; an albedo-only renderer may be placed
anywhere.

## The scene table

`python launcher/tools/r3d/scene_table.py SCENE.scene.toml` reads the scene and
import files without baking and writes `<scene>_scene_generated.c` and `.h`
beside the scene file. A test checks that the committed table matches it.

The table holds only what the device reads: one const `scene_def_t`, registered
by scene name so `scene_load()` finds it ([Scene-Manager.md](Scene-Manager.md)).
It includes each mesh renderer and camera in file order, their transforms, mesh
asset ids, and the camera's lens, background and path symbols. Lights, camera
region, sky, ambient and tone map stay offline.
