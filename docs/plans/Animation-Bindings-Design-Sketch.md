# Animation clips as bindings: design sketch

**Status:** approved; step 1 (TRCK v2, `anim_bind`) is built: its entry and API
are in [Animation-Tracks.md](../Animation-Tracks.md) and `anim/anim_binding.h`;
steps 2-4 are not built.

A clip is a set of bindings: each names the object path, component and field
a curve drives. `anim_bind()` resolves a scene clip against caller-supplied
objects and field declarations once, so no string is compared per frame.
Scene animators, boot binding and skeleton playback are planned below.

The curves use `anim_track_t`, `anim_track_sample()` and glTF interpolations.

## 1. A binding

| Part | Meaning | Example |
|---|---|---|
| `path` | the object, by its name in the scene (and, once scene objects nest, the names down to it) | `"camera"`, `"root/spine/neck"` |
| `component` | which of that object's components | `transform`, `camera`, `skeleton` |
| `field` | which animatable field of it | `position`, `rotation`, `scale`, `half_fov_short_tan` |
| `type` | the value: `float`, `vec2`, `vec3`, `quat`, `colour` (linear RGB, 3 floats) | `quat` |
| `interp` | as today: step, linear, cubic | linear |

Paths resolve **at the scene level** (maintainer): a path names objects in
the scene by their scene names, since the engine has no prefabs or instances
yet. The glTF channel `camera/translation` becomes the binding (`camera`,
`transform`, `position`), resolved against the scene object named `camera`.
A scene binding's path is stored relative to the scene itself; when prefabs
or instancing arrive, the same path can be resolved
under an instance's root instead, with no format change. Designed and tested
against scenes now.

A **skeleton clip** (maintainer) resolves relative to the
skeleton it plays on instead: its paths are joint paths from the skeleton's
root (`root/spine/chest`), so one clip plays on any entity with that rig. The
entry's header says which root a clip's paths start from
(`Animation-System-Design-Sketch.md`, section 3).

## 2. The pack entry, TRCK version 2

The layout and reader checks are in
[The pack entry](../Animation-Tracks.md#the-pack-entry) and
`launcher/main/anim/anim_tracks.h`.

**The baker** (`tracks_asset.py`) maps glTF to bindings in the field's own
units, so the device never converts:

| glTF channel | Binding |
|---|---|
| node `N` translation, rotation, scale | (path of `N`, `transform`, `position` / `rotation` / `scale`) |
| `/cameras/i/perspective/yfov` (animation pointer) | (path of the node holding camera `i`, `camera`, `half_fov_short_tan`), converted from the vertical full angle [A] |
| a camera keys file | `transform` `position` and `rotation`, through glTF as today |
| a pointer to anything no component exposes | the bake fails, naming the channel |

A node's path is its glTF name, which must be the name of the scene object
it drives. A clip whose channels all drive a skin's joints is a skeleton
clip: each path is the joint names from the skeleton's root down.

## 3. What a component lets be animated

Each component with animatable fields lists them where it is defined. The list
is the component's own declaration of its state, kept beside its struct, and
nothing outside it can be bound:

The declaration types are in `launcher/main/anim/anim_binding.h`; the camera's
field list is `R3D_SCENE_CAMERA_FIELDS` in `launcher/main/render/r3d_scene.h`.
See [Playing a new property](../Animation-Tracks.md#playing-a-new-property).

Lights are baked offline and materials are baked into vertex colours, so the
device has neither yet. A clip that binds a light's intensity today fails at
load with the binding named; it is not skipped. When a runtime light or
material component lands, its field list is what makes it animatable.

## 4. Resolve once, sample every frame

The scene's animation system (planned separately: an animator component that
plays a clip on an entity) owns playback. This sketch gives it the resolved
form:

The resolved form and binding API are in `launcher/main/anim/anim_binding.h`,
documented in [The pack entry](../Animation-Tracks.md#the-pack-entry).
`anim_bind()` looks up paths in the caller's target table; `anim_bind_field()`
finds components and fields and checks their types. `anim_apply()` samples
curves into fields and sets supplied dirty bits. The sampler normalizes
quaternions. A scene animator's bound array is planned to live in the scene's
allocation, sized from the clip's binding count at load.

Writing a transform field needs each entity's transform held as position,
rotation and scale, not only as the 3x3 placement the raster reads. That
float transform per entity is the animation system's change; this sketch
assumes it [A], and the placement is built from it when the entity is dirty.

## 5. The cases it must carry

| Case | How |
|---|---|
| Camera path, today's only player | the camera entity gets an animator playing its clip. The scene entry's camera row loses `clip` and `node`; an animator row (`entity`, clip id, wrap) replaces them. `r3d_scene_path_t` and the path half of `r3d_scene_camera_*` are deleted |
| Lens animation | a `camera` `half_fov_short_tan` binding on the camera entity |
| Boot animation (a camera and a space transform) | boot is not a scene, so it passes `anim_bind` a small table of its two objects instead of a scene [A]; its clip and keys need no new source |
| Skinned clips | a `skeleton` component on the mesh entity exposes its joints as a subtree: `path` continues from the entity into joint names (`root/spine/neck`), `component` is `transform`. Joint bindings resolve once. Blending two clips samples each into scratch values and mixes them before writing; that belongs to the skinning work, and the resolved form allows it |
| Unknown path, component or field, or a type mismatch | the scene load fails: `SCENE_ERR_ASSET`, `ASSET_ERR_FORMAT`, the clip id, and the binding as `path:component.field` [A] |

## 6. Order

1. **Format:** TRCK version 2, the baker's mapping, the reader, and
   `anim_bind`'s walk over a test object table, with host tests for every type,
   a bad string offset, an unknown component and a type mismatch. Current
   callers select (`path`, `TRNS`, `position` / `rotation` / `scale`) through
   `anim_tracks_find_node()`; they sample those curves directly.
2. **The animation system** (separate work) adds animators and the float
   transform and moves the camera path onto them; that lookup and
   `r3d_scene_path_t` go.
3. **Boot** resolves its two objects through `anim_bind`.
4. **Skinning** adds the `skeleton` component and its joint subtree.

## Decisions for the maintainer

1. Bindings are (`path`, `component`, `field`), resolved once at load, and only
   fields a component declares can be bound (sections 1, 3).
2. TRCK version 2 stores names in a string table (section 2).
3. The baker converts to the field's units, e.g. glTF `yfov` to
   `half_fov_short_tan`, so the device never converts (section 2).
4. Property clips resolve at the scene level, by scene object name; skeleton
   clips resolve relative to their skeleton, by joint path (maintainer). The stored path can be resolved under an instance root later
   with no format change (section 1).
5. A binding that does not resolve fails the scene load and is never skipped
   (section 3).
