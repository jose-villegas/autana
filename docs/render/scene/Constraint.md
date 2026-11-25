# Position and aim constraints

Each constraint object names its sources in the scene file; the scene resolves them at load.
Timeline curves sample weights at the current time, so seek and playback give the same pose.

In the scene file, attach a position constraint to the camera.

```toml
[[objects]]
name = "camera"

[objects.position_constraint]
sources = [{ target = "follow_target" }, { target = "next_target", weight = 0.0 }]
```

In its timeline's [keys file](../animation/Animation-Tracks.md#keys-files),
cross the two source weights to transfer influence.

```toml
animation = "change_target"

[[tracks]]
object = "camera"
field = "CPOS.weight0"
interp = "linear"
keys = [{ t = 0, v = 1 }, { t = 1, v = 0 }]

[[tracks]]
object = "camera"
field = "CPOS.weight1"
interp = "linear"
keys = [{ t = 0, v = 0 }, { t = 1, v = 1 }]
```

## Position constraint

The position constraint blends the owner's position toward the weighted
average of its sources. Each source contributes its world position plus
the translation offset in its binding frame. Source weights are divided
by their sum. A zero sum leaves the owner's position in place.
<sup>[[144]](../../Citations.md#unity-position-constraint)</sup>
<sup>[[148]](../../Citations.md#unity-constraints)</sup>

| Field | Meaning |
|---|---|
| `weight` | Influence from 0 to 1. Defaults to 1. Zero gives the owner's pose.<sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `sources` | Ordered sources up to `SCENE_CONSTRAINT_SOURCES_MAX`.<sup>[[145]](../../Citations.md#unity-position-constraint-api)</sup> |
| `sources[].target` | Name of a scene object.<sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `sources[].weight` | Defaults to 1. The engine clamps source weights to 0..1 and normalises them by their sum.<sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `translation_offset` | Offset from each source in its binding frame. Defaults to zero.<sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `binding_mode` | `world_space` (default), `lock_to_target` for target rotation, or `lock_to_target_with_world_up` for target yaw.<sup>[[149]](../../Citations.md#unity-cinemachine-binding)</sup> |

Cinemachine Follow's Position Damping (Tracker settings) is not carried
over: it depends on the previous frame, so a seek would differ from playback.
<sup>[[149]](../../Citations.md#unity-cinemachine-binding)</sup>

## Aim constraint

The aim constraint runs after position. It aims toward the weighted
average of the source positions, applies the rotation offset, then blends
from the owner's rotation. A zero weight sum or a coincident aim point
leaves the owner's rotation in place.
<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup>

| Field | Meaning |
|---|---|
| `weight` | Influence from 0 to 1. Defaults to 1. Zero gives the owner's rotation.<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `sources` | Ordered sources up to `SCENE_CONSTRAINT_SOURCES_MAX`.<sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `sources[].target` | Name of a scene object.<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `sources[].weight` | Defaults to 1. The engine clamps source weights to 0..1 and normalises them by their sum.<sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `aim_vector` | Local aim axis. Defaults to `[0, 0, -1]`, the forward of a right-handed source. The pack mirrors it to the engine's +Z ([conventions](../../math/Conventions.md)).<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `up_vector` | Local up axis. Defaults to `[0, 1, 0]`.<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `world_up_type` | How to choose the world up reference, described below.<sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `world_up_vector` | Nonzero reference vector for `vector` and `object_rotation_up`. Defaults to `[0, 1, 0]`.<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `world_up_object` | Required scene object for `object_up` and `object_rotation_up`.<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `rotation_offset` | Euler degrees in the same order as `rotation`. Defaults to zero.<sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |

| World up type | Reference |
|---|---|
| `scene_up` (default) | +Y. |
| `object_up` | Direction from the constrained position to the up object. |
| `object_rotation_up` | The up object's world rotation applied to `world_up_vector`. |
| `vector` | `world_up_vector` in world space. |
| `none` | With no up reference, turn the owner by the shortest rotation that carries its current `aim_vector` onto the target. The `up_vector` is unused. |

## Timeline and ownership

- **Owner.** Position owns the object's position. Aim owns its rotation.
- **Sampled inputs.** A timeline can sample the sources' and up object's transforms.
  The constraint's float curves are `weight` and `weight<i>` for each source index `i`
  below `SCENE_CONSTRAINT_SOURCES_MAX`.
- **Load failure.** Scene loading fails in these cases.<sup>[[148]](../../Citations.md#unity-constraints)</sup>
  - A timeline or property animator writes the claimed field.
  - A constraint drives an owner ancestor, a source, its ancestor, or the up object or its ancestor.
  - A property animator drives a transform on those paths.

A constraint re-solves when the timeline applies a sample or a row on an input path has been placed.
