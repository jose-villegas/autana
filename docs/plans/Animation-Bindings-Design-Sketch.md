# Animation clips as bindings: design sketch

**Status:** for approval, not built. `[A]` marks a proposal of this sketch
that nobody asked for.

A clip today is a list of tracks named by their glTF channel
(`camera/translation`, `lens/perspective/yfov`), and the one caller that plays
one, the scene camera, looks two of them up by string. This sketch makes a clip
a set of **bindings**: each says which field of which object a curve drives.
A clip can then animate any field a component lets be animated (an entity's
position, rotation or scale, a camera's lens, a skeleton's joints), and it is
bound to those fields once, when its scene loads, so no string is compared per
frame.

The curves do not change: `anim_track_t`, `anim_track_sample()` and the glTF
interpolations stay. What changes is how a curve is named, how it is stored,
and who resolves it.

## 1. A binding

| Part | Meaning | Example |
|---|---|---|
| `path` | the object, by its name in the scene (and, once scene objects nest, the names down to it) | `"camera"`, `"capybara/spine/neck"` |
| `component` | which of that object's components | `transform`, `camera`, `skeleton` |
| `field` | which animatable field of it | `position`, `rotation`, `scale`, `half_fov_short_tan` |
| `type` | the value: `float`, `vec2`, `vec3`, `quat`, `colour` (linear RGB, 3 floats) | `quat` |
| `interp` | as today: step, linear, cubic | linear |

Paths resolve **at the scene level** (maintainer): a path names objects in
the scene by their scene names, since the engine has no prefabs or instances
yet. The glTF channel `camera/translation` becomes the binding (`camera`,
`transform`, `position`), resolved against the scene object named `camera`.
The path is stored relative to a root that is, for now, always the scene
itself; when prefabs or instancing arrive, the same path can be resolved
under an instance's root instead, with no format change. Designed and tested
against scenes now.

## 2. The pack entry, TRCK version 2

Little-endian, offsets from the entry's first byte. Names move to a string
table, so a path is not limited to 31 bytes and a repeated name is stored once:

| Part | Layout |
|---|---|
| header | `u16 version` (2), `u16 binding_count`, `u32 duration_ms`, `u32 strings_off`, `u32 strings_size` |
| row per binding, 24 bytes | `u16 path` (string offset), `u16 field` (string offset), `u32 component` (four characters, as a pack entry type is), `u32 times_off`, `u32 values_off`, `u16 count`, `u8 type`, `u8 interp`, 2 zero bytes |
| strings | NUL-terminated, each name once |
| data | `f32` times, then `f32` values, as today, 4-byte aligned |

Width and the quaternion flag follow from `type`, so they are not stored. The
reader checks every row once on open, as now: offsets in range, each string
terminated inside the table, a known `type` and `interp`, `values` sized for
both. A version 1 entry is refused (`ASSET_ERR_VERSION`): packs are build
products, so nothing old needs reading.

**The baker** (`tracks_asset.py`) maps glTF to bindings in the field's own
units, so the device never converts:

| glTF channel | Binding |
|---|---|
| node `N` translation, rotation, scale | (path of `N`, `transform`, `position` / `rotation` / `scale`) |
| `/cameras/i/perspective/yfov` (animation pointer) | (path of the node holding camera `i`, `camera`, `half_fov_short_tan`), converted from the vertical full angle [A] |
| a camera keys file | `transform` `position` and `rotation`, through glTF as today |
| a pointer to anything no component exposes | the bake fails, naming the channel |

A node's path is its glTF name, which must be the name of the scene object
it drives, or for a node inside a rigged model, the model's scene object name
followed by the joint names down to it [A].

## 3. What a component lets be animated

Each component with animatable fields lists them where it is defined. The list
is the component's own declaration of its state, kept beside its struct, and
nothing outside it can be bound:

```c
/* anim/anim_binding.h */
typedef enum { ANIM_FLOAT, ANIM_VEC2, ANIM_VEC3, ANIM_QUAT, ANIM_COLOUR } anim_value_t;

typedef struct {
    const char* name;   /* "position" */
    anim_value_t type;
    uint16_t offset;    /* bytes into the component's struct */
} anim_field_t;

typedef struct {
    uint32_t component; /* four characters, e.g. 'T','R','N','S' */
    const anim_field_t* fields;
    uint8_t field_count;
} anim_component_fields_t;

/* scene/: beside each component's struct */
extern const anim_component_fields_t SCENE_TRANSFORM_FIELDS; /* position vec3, rotation quat, scale vec3 */
extern const anim_component_fields_t SCENE_CAMERA_FIELDS;    /* half_fov_short_tan, near_z float; clear colour */
```

Lights are baked offline and materials are baked into vertex colours, so the
device has neither yet. A clip that binds a light's intensity today fails at
load with the binding named; it is not skipped. When a runtime light or
material component lands, its field list is what makes it animatable.

## 4. Resolve once, sample every frame

The scene's animation system (planned separately: an animator component that
plays a clip on an entity) owns playback. This sketch gives it the resolved
form:

```c
/* One binding, resolved: where its sampled value goes. */
typedef struct {
    anim_track_t curve;     /* points into the pack entry */
    float* target;          /* the field inside one component array element */
    anim_value_t type;
    scene_entity_t entity;  /* whose dirty flag a write sets */
} anim_bound_t;

/* Resolves every binding of `clip` against the scene's object names. On the
 * first that does not resolve it returns ANIM_BIND_ERR_* and that binding's
 * index, so the scene load fails naming the clip and the binding. A root
 * argument joins this signature when prefabs or instances exist. */
anim_bind_status_t anim_bind(const anim_tracks_t* clip, const scene_t* scene,
                             anim_bound_t* out, int out_count, int* failed);

/* Per animator per frame: one clip time, then each curve sampled straight
 * into its target. */
void anim_apply(const anim_bound_t* bound, int count, float seconds);
```

`anim_bind` looks `path` up among the scene's object names, finds `component` on the entity
it reaches, then `field` in that component's list, and checks the type. It is
the only place strings are compared. `anim_apply` samples each curve, writes
its floats to `target`, normalises a `quat`, and sets the entity's dirty flag so
its placement is rebuilt. The bound array lives in the scene's one allocation,
sized from the clip's binding count at load.

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
| Skinned clips | a `skeleton` component on the mesh entity exposes its joints as a subtree: `path` continues from the entity into joint names (`spine/neck`), `component` is `transform`. 22 joints, 3 bindings each, resolved once. Blending two clips samples each into scratch values and mixes them before writing; that belongs to the skinning work, and the resolved form allows it |
| Unknown path, component or field, or a type mismatch | the scene load fails: `SCENE_ERR_ASSET`, `ASSET_ERR_FORMAT`, the clip id, and the binding as `path:component.field` [A] |

## 6. Order

1. **Format:** TRCK version 2, the baker's mapping, the reader, and
   `anim_bind`'s walk over a test object table, with host tests for every type,
   a bad string offset, an unknown component and a type mismatch. Today's
   callers read version 2 through a lookup that answers `node/translation` with
   the node's (`path`, `transform`, `position`) binding, so nothing else changes
   yet.
2. **The animation system** (separate work) adds animators and the float
   transform and moves the camera path onto them; that lookup and
   `r3d_scene_path_t` go.
3. **Boot** resolves its two objects through `anim_bind`.
4. **Skinning** adds the `skeleton` component and its joint subtree.

## Decisions for the maintainer

1. Bindings are (`path`, `component`, `field`), resolved once at load, and only
   fields a component declares can be bound (sections 1, 3).
2. TRCK version 2 replaces version 1 outright and stores names in a string
   table (section 2).
3. The baker converts to the field's units, e.g. glTF `yfov` to
   `half_fov_short_tan`, so the device never converts (section 2).
4. Paths resolve at the scene level, by scene object name (maintainer,
   confirmed); the stored path can be resolved under an instance root later
   with no format change (section 1).
5. A binding that does not resolve fails the scene load and is never skipped
   (section 3).
