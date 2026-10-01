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
path = { tracks = "flight", node = "camera" }

[[objects]]
name = "hall"

[objects.mesh_renderer]
mesh = "hall.import.toml"
variant = "hall"                 # only for an import with variants

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
| `mesh_renderer` | `mesh`, `variant` | Draws a mesh asset: `mesh` names an import file beside the scene file, which must exist, and `variant` picks one of its variants (required exactly when the import has them). |
| `light` | `type`, `color`, `intensity`, `disc_degrees`, `rays` | A directional light. The direction toward it is the object's +Y axis turned by its rotation, so a rotation of zero is a sun straight overhead. Position and scale do not matter. `point` and `spot` are reserved and rejected until their bake paths exist. |
| `camera` | `half_fov_short_tan`, `near_z`, `region`, `path` | The view: the lens, the box the camera moves within (`region`, a `min` and `max`), and optionally the glTF animation it flies. `path = { tracks, node }` names the tracks `tools/anim/bake_tracks.py` baked under the prefix `tracks`, for the glTF node `node`. Without a path the camera sits at its transform, looking down its own -Z. A scene has at most one camera. |

Sky and ambient light are properties of the scene, not objects, and are the
two settings tables `[sky]` (`color`, `intensity`, `rays`: that many random
directions per point over the hemisphere) and `[ambient]` (`color`,
`intensity`: a constant added everywhere). The lights a bake sees are the
directional light objects in file order, then the sky, then the ambient; the
order does not change the lit result except in which random rays each light
draws. A double-sided face turns to the side the directional lights, summed by
intensity, shine on.

## What the scene must carry

A mesh whose import has a scene-dependent step reads the scene:

- `process.light` reads the lights (at least one directional object, `[sky]`
  or `[ambient]`) and `tonemap_white`;
- `process.visibility` reads the camera's `region`.

A scene must carry what a placed mesh reads, and may not carry what none
reads. Baking a mesh reads the lights of the scene that requests it, so a mesh
with a scene-dependent step belongs to one scene: two scenes may not bake the
same output name, and a second scene that places it bakes its own variant under
another name. A mesh with a scene-dependent step (`process.light` or
`process.visibility`) is baked where it sits, so its object's transform must be
identity; a mesh without one may be placed anywhere and by many scenes.

## The scene table

`python launcher/tools/r3d/scene_table.py SCENE.scene.toml` reads the scene file
and its import files (it needs no numeric environment and bakes nothing) and
writes `<scene>_scene_generated.c` and `.h` into the output directory its import
files name, which must be the same for all of them. A test fails when the committed table is not
what its scene file generates.

The table holds only what the device reads, as const data from
[`render/r3d_scene.h`](../../launcher/main/render/r3d_scene.h) and
[`render/r3d_instance.h`](../../launcher/main/render/r3d_instance.h):

- one `r3d_instance_t` per mesh renderer, named `<scene>_scene_<object>`, so a
  misspelt object fails at link time, with a view of its mesh and its placement
  baked as a 3x3 (rotation times scale) and a position, or no placement for the
  identity;
- `<scene>_scene_assets`, the asset id of each mesh and the view it fills:
  `r3d_scene_bind()` opens each from the [asset pack](../Asset-Packs.md) before
  the instances are drawn, and an id the pack lacks fails there, naming it;
- the camera, `r3d_scene_camera_t` named `<scene>_scene_<object>`: its lens, its
  placement (a fixed camera looks down the placement's -Z column) and the
  symbols of its path.

Lights, the camera region, sky, ambient and the tone map are bake settings and
stay offline. The rotation convention above is written only in the importer, so
the device does no trigonometry. A scene hands an instance to a raster, and asks
`r3d_scene_camera_at()` for the camera at a time, so what to draw and where lives
in the scene file rather than in code.
