# Scene Files

A scene file holds transformed objects, renderer, light and camera components,
optional animators, and ambient and tone-map settings. The offline importer bakes the lit meshes
the scene places ([Mesh-Import.md](../offline/Mesh-Import.md)); the pack build bakes the
[scene entry](../../assets/formats/SCNE.md) it reads at run time.

```toml
tonemap_white = 0.35             # larger is darker

[sky]                            # scene settings, not objects
color = [0.55, 0.68, 0.9]
intensity = 0.9
rays = 48

[ambient]
color = [1.0, 1.0, 1.0]
intensity = 0.06

[bake]
ray_offset = 0.5
colour_merge_step = 6
indirect = { bounces = 2, rays = 64 }

[[objects]]
name = "sun"
rotation = [18.4, -48.7, 0.0]    # pitch, yaw, roll in degrees

[objects.light]
type = "directional"
color = [1.0, 0.92, 0.78]
intensity = 3.0

[[objects]]
name = "camera"

[objects.camera]
half_fov_short_tan = 0.62
near_z = 6.0
background = 0x9CC0E6
region = { min = [-1400.0, 20.0, -620.0], max = [1270.0, 1250.0, 550.0] }
path = { animation = "../assets/flight.anim.toml", node = "camera" }

[[objects]]
name = "hall"

[objects.mesh_renderer]
mesh = "hall.import.toml"
variant = "hall_fitted_full"     # a variant with `triangles`, the fit's start
bake = true
visibility = { source = "camera_region", rounds = 160 }

[objects.mesh_renderer.fit]      # fitted to reference renders of the source
prune = { budget = 13313, coverage_every_ms = 20 }
poses = { size = [184, 224], train_every_ms = 1000, held_out_every_ms = 5000 }

[objects.mesh_renderer.fit.optimise]
steps = 2000
batch = 8
laplacian = 10.0
normal_weight = 1.0
surface_weight = 10.0
surface_margin = 0.25

[[objects]]
name = "statue"
position = [0.0, 0.0, 300.0]
scale = [2.0, 2.0, 2.0]

[objects.mesh_renderer]          # a flat fit: one colour per face
mesh = "statue.import.toml"
variant = "statue_flat_fitted"
bake = true
shading = { flat = { auto = { min = 1, max = 16, area = "median" } } }
visibility = { source = "camera_region", rounds = 160 }

[objects.mesh_renderer.fit]
prune = { budget = 4000, coverage_every_ms = 20 }
poses = { size = [184, 224], train_every_ms = 1000, held_out_every_ms = 5000 }
optimise = { steps = 2000, batch = 8, laplacian = 10.0, normal_weight = 1.0, surface_weight = 10.0, surface_margin = 0.25 }
```

## Objects

Every object has a unique `name`, an optional transform and one primary
component table: `mesh_renderer`, `skinned_renderer`, `light` or `camera`.
A skinned renderer may also have an animator. A scene needs a mesh renderer
or skinned renderer.

| Key | Meaning |
|---|---|
| `parent` | The name of an object listed before this one, whose transform this one's is relative to. Default: none, a root. |
| `position` | Where the object is, in model units. Default `[0, 0, 0]`. |
| `rotation` | `[pitch, yaw, roll]` in degrees, right-handed: roll about z, then pitch about x, then yaw about y. Default `[0, 0, 0]`. |
| `scale` | Per-axis scale, positive on every axis. Default `[1, 1, 1]`. |

| Component | Fields | What it is |
|---|---|---|
| `mesh_renderer` | `mesh`, `variant`, `bake`, `shading`, `visibility`, `fit`, `indirect` | Draws a mesh asset. `mesh` names an import file beside the scene; `variant` chooses its geometry variant. |
| `skinned_renderer` | `mesh`, `variant` | Draws imported albedo with its skin and skeleton, lit at runtime. Takes no `bake`. |
| `animator` | `clips`, `default_clip`, `wrap` | Selects a skinned renderer's clips and playback. |
| `light` | `type`, `color`, `intensity` | A directional light; see [light](#light). |
| `camera` | `half_fov_short_tan`, `near_z`, `region`, `path`, `background` | The view; see [camera](#camera). Only the active camera renders. |

## What the scene must carry

| A scene with | Must carry | May also carry |
|---|---|---|
| a baked renderer (`bake = true`) | a light (a directional object, `[sky]` or `[ambient]`), `tonemap_white` and `[bake]`; the camera `region` for region visibility, its `path` for path visibility | `[indirect]`, when a renderer bakes `[bake].indirect` |
| skinned renderers only | nothing | directional lights, `[ambient]`, `tonemap_white` |

The importer rejects a light, `tonemap_white`, a region or `[indirect]` that no
placed renderer reads; `[sky]` is read only by a bake.

## Option reference

Keys before `;` are required; after it, optional. The import option reference
is in [Mesh-Import.md](../offline/Mesh-Import.md#import-options).

### Scene settings

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `[bake]` | `ray_offset`, `colour_merge_step`; `indirect = { bounces, rays }`, `ao = { distance, rays; strength, indirect }` | Scene-wide tracing settings. | Required by a baked renderer. | [bake](#bake) |
| `[indirect]` | `; intensity, albedo_boost` | Sets the appearance of the baked bounced light. | `1.0 each`; rejected unless a baked renderer uses [bake].indirect. | [indirect](#indirect) |
| `[sky]` | `color`, `intensity`, `rays` | Hemisphere light for bakes. | Rejected unless a renderer bakes. | [sky](#sky) |
| `[ambient]` | `color`, `intensity` | Constant linear light for bakes and skinned renderers. | Absent: zero intensity. | [ambient](#ambient) |
| `tonemap_white` | value | Tone-map white point for bakes and runtime lighting. | Runtime default `0` (identity); required by a baked renderer. | [tonemap_white](#tonemap_white) |

#### bake

`ray_offset` starts a tracing ray clear of its source surface. On a reduced
source it is at least the reduction's error, or two shells moved across each
other shadow one another. `colour_merge_step` controls colour merging after
the bake. The image compares albedo with the result of a renderer whose
`bake = true` light is then [fitted](Appearance-Fit.md).

![Albedo against the fitted bake](../../images/render/import-light.png)

#### bake: indirect

`[bake].indirect = { bounces = K, rays = R }` adds the light that has bounced off
the source, path-traced against the full-detail mesh with the scene's
directional and sky lights as the emitters. Both fields are required when it
is present.

| Field | Meaning |
|---|---|
| `bounces` | How many times light bounces, at least 1. |
| `rays` | Cosine-weighted hemisphere rays for each point. |

`rays` is at least 1. The reference renderer reads
the same settings. A renderer can opt out with [indirect: off](#indirect-off).

#### bake: ao

`[bake].ao = { distance = D, rays = R }` darkens crevices, contact lines and
spots under overhangs. The sky light is blocked by an occluder at any
distance; this counts only hits within `D` model units. It scales the
[ambient](#ambient) light, and with `indirect = true` the bounced
light. The directional and sky lights are untouched. Every baked renderer gets it. How the factor is computed is in
[Bake-Method.md](../offline/Bake-Method.md#local-occlusion-implementation).

| Field | Meaning |
|---|---|
| `distance` | Reach of the occlusion rays in model units, greater than 0. |
| `rays` | Rays per point, at least 1. |
| `strength` | Optional, 0 to 1, default 1: the factor falls as low as 1 − `strength`. |
| `indirect` | Optional, default false: also scale the bounced light. |

Off unless given. A scene that sets it needs something to scale: an
`[ambient]` light, or `indirect = true` with [bounced light](#bake-indirect)
that a baked renderer uses. `reference_render.py`'s `bake` backend reads the same table;
its `mitsuba` backend ignores it. A bake stores one colour per vertex, so a
`distance` much larger than the triangles darkens whole faces around a crease;
a smaller `distance` or a lower `strength` keeps it where surfaces are close
([measured](../offline/Bake-Quality.md#local-occlusion)).

#### indirect

`intensity` is at least 0 and multiplies the bounced light.
`albedo_boost` is greater than 0 and replaces bounce reflectance with
$`\min(\beta a, \max(a, 0.99))`$. The bake stores the result in the same vertex
or face colours as direct light. The reference renderer reads the same table,
so a scene's reference carries its look; `intensity` or `albedo_boost` above 1
trades fidelity to a physical reference for look.

The look against the reference: [Bake-Quality.md](../offline/Bake-Quality.md#indirect-light).

#### sky

`[sky]` supplies coloured hemisphere light; its `rays` are the fixed
cosine-weighted directions each point tests for sky visibility.

#### ambient

`[ambient]` adds constant linear coloured light to baked and runtime skinned shading.
Colour components are finite in 0..1; intensity is finite and nonnegative.

#### tonemap_white

`tonemap_white` $w$ sets the tone map applied to baked and runtime skinned light: radiance $L$
becomes $L / (1 + wL)$ before the gamma, Reinhard's global operator<sup>[[7]](../../Citations.md#reinhard-2002)</sup>
at $w = 1$ and none at $w = 0$.

### mesh_renderer

An example `fit` recipe, grouped below its renderer's `fit` table:

```toml
[objects.mesh_renderer.fit.prune]
budget = 8672
coverage_every_ms = 100

[objects.mesh_renderer.fit.poses]
size = [184, 224]
train_every_ms = 1000
held_out_every_ms = 5000

[objects.mesh_renderer.fit.optimise]
steps = 2000
batch = 8
laplacian = 10.0
surface_weight = 10.0
surface_margin = 0.25
normal_weight = 1.0
```

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `bake: renderer` | `true` | Traces this renderer against its own source. | `false`. | [bake renderer](#bake-renderer) |
| `variant` | name | Chooses a named geometry variant. | Required when the import has variants. | [variant](#variant) |
| `colour_set` | nonnegative integer | Selects an external LCOL set on a colourless mesh. | Mesh-owned colours. | [colour_set](#colour_set) |
| `visibility: camera_region` | `source = "camera_region"`, `rounds` | Keeps triangles visible from any point in the camera's region. | Off. | [visibility: camera_region](#visibility-camera_region) |
| `visibility: camera_path` | `source = "camera_path"`, `every_ms`, `size`; `samples`, `margin` | Keeps triangles first seen from sampled camera-path views. | Off. | [visibility: camera_path](#visibility-camera_path) |
| `shading: smooth` | `"smooth"` | Stores baked colour at vertices. | `"smooth"`. | [shading: smooth](#shading-smooth) |
| `shading: flat` | `flat = { fixed = N }` / `flat = { auto = { min, max, area } }` | Stores one averaged RGB565 colour per face. | Off. | [shading: flat](#shading-flat) |
| `fit.prune` | `budget`, `coverage_every_ms` | Prunes the fitted mesh to the triangle budget. | Required in `fit`; the cost weight is `fitted_variant.py sweep --cost-weights`, not a scene key. | [fit.prune](#fitprune) |
| `fit.poses` | `size`, `train_every_ms`, `held_out_every_ms` | Selects training and held-out camera-path poses. | Required in `fit`. | [fit.poses](#fitposes) |
| `fit.optimise` | `steps`, `batch`, `laplacian`, `normal_weight`, `surface_weight`, `surface_margin` | Sets optimiser steps, batch size and loss weights. | Required in `fit`. | [fit.optimise](#fitoptimise) |
| `indirect: off` | `indirect = false` | Bakes this renderer without bounce light. | Bounce light on when the scene recipe is present. | [indirect: off](#indirect-off) |

#### bake renderer

`bake = true` writes this renderer's lit mesh; otherwise it draws the shared
imported albedo mesh. `shading`, `visibility`, `fit`, and `indirect` require
`bake = true`. A baked renderer's transform must be identity. Its output is
`<scene>.<object>.mesh`; an albedo renderer uses `<variant>.mesh`. An import is
packed only through the scenes that place it. The same import can have
independent bakes in several scenes; an albedo renderer's mesh is shared and
may be placed anywhere, by many scenes.

#### variant

`variant` selects a named geometry output from its import.

#### colour_set

`colour_set` is a uint16 index into `<mesh id>.colors`; the
`SCENE_ASSET_COLOUR_SET_OWN` sentinel selects mesh-owned colours. Pack
validation checks ownership, set range, encoding and element count.

#### visibility: camera_region

`source` defaults to `"camera_region"`. The region source casts from points in
the camera's `region`; use it when the camera may occupy the box without a
defined path. `rounds` is how many random tries each triangle gets. A try tests
one point of the triangle plus one for each whole multiple of the mean triangle area,
so a large face is kept by how much of it shows, not by where a single point
lands.

The image is the same import with the cull off above and on below, cropped
where they differ most: without the cull the budget goes to hidden surfaces.

![Visibility cull off against on](../images/import-visibility.png)

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

The views are square, as wide as the longer side of `size`, so the panel held
either way up is covered; `margin` and the pose spacing cover geometry that
enters between samples.

#### shading: smooth

Smooth shading bakes one colour per vertex. It is the normal baked mesh form
described in [The baked mesh](../offline/Baked-Mesh.md#clusters-and-the-tree).

#### shading: flat

Flat shading stores one RGB565 colour per triangle. `fixed` uses that many
lighting points per face; `auto` chooses a count from face area. One point lights a face
from one place, so a shadow edge lands on whole faces
([smooth against flat](../offline/Bake-Quality.md#the-demo-variants)).

A flat fit starts from these sampled colours, then fits them. Top: one fixed
sample against adaptive on the fit's start mesh. Bottom: adaptive against the
finished fit.

![One fixed face sample against adaptive, then adaptive against the flat fit](../../images/render/import-face-samples.png)

Face samples are the useful control; extra samples converge, and a face's one
colour leaves edge error that sampling cannot remove. A flat bake's sky rays
are the sky light's `rays`. Sweeps, and `bake_fidelity.py` to score your own
settings, are in the scene tools README.

#### fit.prune

`budget` is the triangle count the fitted mesh keeps, at most the variant's
`triangles`; `coverage_every_ms` spaces the camera-path poses pruning counts
pixels over. How a fit runs: [Appearance-Fit.md](../offline/Appearance-Fit.md#stages).

#### fit.poses

`size = [width, height]` sets the pose resolution; `train_every_ms` spaces the
training poses along the camera path, and poses at multiples of
`held_out_every_ms` are held out and only scored ([Poses](../offline/Appearance-Fit.md#poses)).

#### fit.optimise

`steps` and `batch` set the optimiser's length and poses per step.
`laplacian` keeps each vertex's offset from its neighbours' average close to
the start's, so local detail stays put; `normal_weight` makes
normals agree with the source's, and `surface_weight` keeps the mesh near the source surface, beyond
`surface_margin` local edge lengths from it. Each weighs a term of
[the loss](../offline/Appearance-Fit.md#the-loss); with `shading = { flat = ... }` the fit
is [flat](../offline/Appearance-Fit.md#a-flat-fit).

#### indirect: off

`indirect = false` requires `[bake].indirect` and omits its bounces from this
renderer and its fit reference.

### skinned_renderer and animator

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `skinned_renderer` | `mesh`; `variant` | Draws imported albedo with its skin and skeleton, lit at runtime. | Takes no `bake`. | [skinned_renderer](#skinned_renderer) |
| `animator` | ; `clips`, `default_clip`, `wrap` | Selects a skinned renderer's clips and playback. | The import's `source.clips`; no default clip; `loop`. | [animator](#animator) |

#### skinned_renderer

```toml
[[objects]]
name = "subject"
[objects.skinned_renderer]
mesh = "subject.import.toml"

[objects.animator]
default_clip = "idle"
wrap = "loop"
# clips = ["idle", "walk"]
```

The mesh id is `<scene>.<object>`; the skin id appends `.skin` and the skeleton
id comes from the imported rig. The mesh holds unlit albedo. `bake` is refused.

#### animator

`clips` defaults to the import's `source.clips`; an explicit list selects the
animator's clips. `default_clip` names one of those clips; absent, nothing plays.
`wrap` is `loop` (default) or `clamp`. Without an animator the skin uses its
rest pose. Each component occurs once per entity.

### Light options

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `light` | `type`, `color`, `intensity` | A directional light: baked into baked renderers, applied at runtime to skinned renderers. | Required for an object with this component. | [light](#light) |

#### light

Only directional lights are scene objects; `point` and `spot` are reserved.
Its direction toward the light is the object's +Y axis turned by its rotation.
Position and scale do not matter. The lights a bake sees are the directional
objects in file order, then `[sky]`, then `[ambient]`. The sun is a point
source, so its shadows are hard. A double-sided face is lit on both sides and
keeps the brighter.

### Camera options

| Option | Keys | What it does | Default | Option link |
|---|---|---|---|---|
| `camera` | `half_fov_short_tan`, `near_z`; `region`, `path`, `background` | Defines the scene view. | — | [camera](#camera) |

#### camera

A scene may hold many cameras; only one renders at a time, the active one
selected by `scene_activate()`. Camera-region visibility, camera-path
visibility and fitted variants read the scene's first camera. A region on
any other camera is refused because no bake step reads it.

| Key | Meaning |
|---|---|
| `region = { min, max }` | the box the camera may occupy; region visibility casts from points inside it |
| `path = { animation, node }` | the clip the camera flies: `animation` is its `NAME.anim.toml` ([Animation-Tracks.md](../animation/Animation-Tracks.md)), relative to the scene file, and `node` the glTF node in it that is the camera. The clip's id is `NAME` and it travels in the scene's pack; `<node>/translation` must fit a track name's 31 bytes |
| `background` | 0xRRGGBB, default black: the colour a pixel no mesh covers shows, on the panel and in the source reference |

Without a path the camera sits at its transform, looking down its own -z in the
source frame; the pack carries it in the engine frame ([Mesh-Import.md](../offline/Mesh-Import.md#the-offline-tools)).
