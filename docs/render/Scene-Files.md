# Scene Files

A scene file is a scenario: a list of **objects**, each with one transform and
one component, plus the settings of the light that fills the air and the tone
map. The offline importer is run on it to bake the lit meshes the scenario places
([Mesh-Import.md](Mesh-Import.md)), and `scene_table.py` writes the
[scene table](#the-scene-table) a scene reads at run time to know what to draw
and where.

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
flat_sky_rays = 128
indirect = { bounces = 2, rays = 64, cache_samples = 1 }

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
background = 0x9CC0E6
region = { min = [-1400.0, 20.0, -620.0], max = [1270.0, 1250.0, 550.0] }
path = { tracks = "flight", node = "camera" }

[[objects]]
name = "hall"

[objects.mesh_renderer]
mesh = "hall.import.toml"
variant = "hall"                 # only for an import with variants
bake = true
visibility = { source = "camera_region", rounds = 160 }

[[objects]]
name = "statue"
position = [0.0, 0.0, 300.0]
scale = [2.0, 2.0, 2.0]

[objects.mesh_renderer]
mesh = "statue.import.toml"
```

## Objects

Every object has a unique `name`, an optional transform and exactly one
component table.

| Key | Meaning |
|---|---|
| `position` | Where the object is, in model units. Default `[0, 0, 0]`. |
| `rotation` | `[pitch, yaw, roll]` in degrees, right-handed: roll about z, then pitch about x, then yaw about y. Default `[0, 0, 0]`. |
| `scale` | Per-axis scale, positive on every axis (the importer rejects zero and negative). Default `[1, 1, 1]`. |

| Component | Fields | What it is |
|---|---|---|
| `mesh_renderer` | `mesh`, `variant`, `bake`, `shading`, `visibility`, `fit`, `indirect` | Draws a mesh asset. `mesh` names an import file beside the scene file, and `variant` chooses its geometry variant. `bake = true` traces this renderer against its own source using this scene's settings; without it the shared imported albedo mesh is drawn. `shading` is `"smooth"` or `{ flat = ... }`; `visibility`, `fit` and `indirect = false` apply to this renderer's bake. |
| `light` | `type`, `color`, `intensity`, `disc_degrees`, `rays` | A directional light. The direction toward it is the object's +Y axis turned by its rotation, so a rotation of zero is a sun straight overhead. Position and scale do not matter. `point` and `spot` are reserved and rejected until their bake paths exist. |
| `camera` | `half_fov_short_tan`, `near_z`, `region`, `path`, `background` | The view: the lens, the box the camera moves within (`region`, a `min` and `max`), and optionally the glTF animation it flies. `path = { tracks, node }` names the tracks `tools/anim/bake_tracks.py` baked under the prefix `tracks`, for the glTF node `node`; the generated `<tracks>_tracks_generated.{c,h}` sit beside the scene file. `background` (0xRRGGBB, default black) is the colour a pixel no mesh covers shows, in the panel's RGB565 and in the source reference. Without a path the camera sits at its transform, looking down its own -Z. A scene has at most one camera. |

An appearance-fit recipe is grouped below its renderer's `fit` table.

| Group | Key | Meaning |
|---|---|---|
| `target` | `budget`, `coverage_every_ms` | Triangle budget and camera-path sampling interval used to choose triangles. |
| `train` | `train_every_ms` | Camera-path interval for training poses. |
| `score` | `held_out_every_ms` | Interval selecting held-out poses from the training samples. |
| `optimise` | `steps`, `batch`, `laplacian`, `normal_weight` | Optimiser iteration count, batch size and loss weights. |
| `output` | `sha256`, `recipe_sha256` | Hashes of the fitted mesh and its effective recipe. |

Sky and ambient light are properties of the scene, not objects, and are the
two settings tables `[sky]` (`color`, `intensity`, `rays`: that many random
directions per point over the hemisphere) and `[ambient]` (`color`,
`intensity`: a constant added everywhere). The lights a bake sees are the
directional light objects in file order, then the sky, then the ambient; the
order does not change the lit result except in which random rays each light
draws. A double-sided face turns to the side the directional lights, summed by
intensity, shine on.

## Bake options

`[bake]` supplies `ray_offset` and `colour_merge_step`; `flat_sky_rays` is
required by a flat renderer, and `indirect = { bounces, rays, cache_samples }`
enables the scene's bounce-light cache. All three indirect fields are required;
`bounces` is non-negative and the sample counts are positive. `indirect = false`
on a renderer omits that cache from its bake and fit reference.

| Renderer option | Keys | Meaning |
|---|---|---|
| `[bake]` | `ray_offset`, `colour_merge_step`; `flat_sky_rays`, `indirect = { bounces, rays, cache_samples }` | Scene-wide tracing settings. Flat renderers require `flat_sky_rays`; indirect is the cache recipe. |
| `visibility` | `source = "camera_region"`, `rounds` | Keeps triangles visible from any point in the camera's region. |
| `visibility` | `source = "camera_path"`, `every_ms`, `size`; `samples`, `margin` | Keeps triangles first seen from sampled camera-path views. The measured path retains 116,917 of 245,465 source triangles rather than 211,004 for the region, reducing the full mesh from 58.6 to 50.1 ms with no lite holes and 4–8 pixels in each of three full frames. |
| `shading` | `"smooth"` or `flat = { fixed = N }` / `flat = { auto = { min, max, area } }` | Smooth stores vertex colour; flat stores one averaged RGB565 colour per face. |
| `fit` | `budget`, pose spacing, optimiser settings and hashes | Records the smooth appearance-fit recipe for this renderer. |

Camera-path visibility uses a square view as wide as the longer panel side, so
both orientations are covered. Its margin and pose spacing cover geometry that
enters between samples.

![Triangles the camera path never sees](images/appearance-path-culled.png)
![Culled lite against uncut, largest differences](images/appearance-path-culled.crops.png)

![Albedo against baked light](images/import-light.png)
![One fixed face sample against adaptive](images/import-face-samples.png)

## Indirect look

`[indirect]` sets how the baked bounce light looks. `[bake]` supplies the
direct-light and cache settings: `ray_offset`, `colour_merge_step`, optional
`flat_sky_rays`, and optional `indirect = { bounces, rays, cache_samples }`.
Both tables are scene settings because the lights and camera determine their
output.

| Key | Meaning |
|---|---|
| `intensity` | A multiplier on the gathered bounce light, at least 0. Above 1 brightens what bounces. |
| `albedo_boost` | A multiplier on the reflectance every bounce uses, above 0, held below 0.99 and never below the surface's own albedo. Above 1 carries more of a surface's colour to its neighbours. |

The reference renderer reads the same table, so a scene's reference carries
its look; values above 1 trade fidelity to a physical reference for look. The
table is read by no mesh unless a placed import has `indirect`, and is rejected
then.

## What the scene must carry

A renderer with `bake = true` reads the scene:

- `[bake]` reads the lights (at least one directional object, `[sky]` or
  `[ambient]`), `tonemap_white`, and its `[indirect]` look;
- renderer `visibility` reads the camera's `region`, or its `path` for the
  `camera_path` source.

A scene must carry what a baked renderer reads, and may not carry what none
reads. A baked scene output is `<scene>.<object>.mesh`, beside the scene file,
so the same import can have independent bakes in several scenes. An albedo
renderer uses `<variant>.mesh` beside its import and shares it across scenes. An import is packed only through the scenes that place it. A baked renderer
is baked where it sits, so its object's transform must be identity; an
albedo-only renderer may be placed anywhere and by many scenes.

## The scene table

`python launcher/tools/r3d/scene_table.py SCENE.scene.toml` reads the scene file
and its import files (it needs no numeric environment and bakes nothing) and
writes `<scene>_scene_generated.c` and `.h` beside the scene file. A test fails
when the committed table is not what its scene file generates.

The table holds only what the device reads: one const `scene_def_t` named
`<scene>_scene`, registered by the scene's name with `SCENE_REGISTER()` so that
`scene_load()` finds it ([Scene-Manager.md](Scene-Manager.md)):

- an entity for each mesh renderer and the camera, in file order; the header
  names each `<SCENE>_SCENE_<OBJECT>`, so a misspelt object fails to compile;
- each entity's transform, baked as a 3x3 (rotation times scale) and a
  position, the identity written out;
- each mesh renderer's asset id: `scene_load()` opens it from the
  [asset pack](../assets/README.md), and an id the pack lacks fails the load,
  naming it;
- the camera's lens, its background colour and the symbols of its path.

Lights, the camera region, sky, ambient and the tone map are bake settings and
stay offline. The rotation convention above is written only in the importer, so
the device does no trigonometry, and what to draw and where lives in the scene
file rather than in code.
