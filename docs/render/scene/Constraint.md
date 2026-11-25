# Position and aim constraints

The scene row wires each constraint to its sources. Timeline curves sample
weights at the current time, so seek and playback give the same pose.
Cross two source weights to transfer influence between them.
See the [TOML example](Scene-Files.md#constraint-and-hinge).

## Position constraint

The position constraint blends the owner's position toward the weighted
average of its sources. Each source contributes its world position plus
the translation offset in its binding frame. Source weights are divided
by their sum. A zero sum leaves the owner's position in place.
<sup>[[144]](../../Citations.md#unity-position-constraint)</sup>
<sup>[[148]](../../Citations.md#unity-constraints)</sup>

| Field | Meaning |
|---|---|
| `weight` | Influence from 0 to 1. Defaults to 1. Zero gives the owner's pose. <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> <sup>[[145]](../../Citations.md#unity-position-constraint-api)</sup> |
| `sources` | Ordered sources up to `SCENE_CONSTRAINT_SOURCES_MAX`. <sup>[[145]](../../Citations.md#unity-position-constraint-api)</sup> <sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `sources[].target` | Name of a scene object. <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `sources[].weight` | Relative influence from 0 to 1. Defaults to 1. <sup>[[148]](../../Citations.md#unity-constraints)</sup> <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> <sup>[[145]](../../Citations.md#unity-position-constraint-api)</sup> |
| `translation_offset` | Offset from each source in its binding frame. Defaults to zero. <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> <sup>[[145]](../../Citations.md#unity-position-constraint-api)</sup> |
| `binding_mode` | `world_space` (default), `lock_to_target` for target rotation, or `lock_to_target_with_world_up` for target yaw. <sup>[[149]](../../Citations.md#unity-cinemachine-binding)</sup> |

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
| `weight` | Influence from 0 to 1. Defaults to 1. Zero gives the owner's rotation. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `sources` | Ordered sources up to `SCENE_CONSTRAINT_SOURCES_MAX`. <sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `sources[].target` | Name of a scene object. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `sources[].weight` | Relative influence from 0 to 1. Defaults to 1. <sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `aim_vector` | Local aim axis. Defaults to authored `[0, 0, -1]`. The pack mirrors it to engine +Z. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `up_vector` | Local up axis. Defaults to `[0, 1, 0]`. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `world_up_type` | How to choose the world up reference, described below. <sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `world_up_vector` | Nonzero reference vector for `vector` and `object_rotation_up`. Defaults to `[0, 1, 0]`. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> <sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `world_up_object` | Required scene object for `object_up` and `object_rotation_up`. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> <sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `rotation_offset` | Euler degrees in the same order as `rotation`. Defaults to zero. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |

| World up type | Reference |
|---|---|
| `scene_up` (default) | +Y. |
| `object_up` | Direction from the constrained position to the up object. |
| `object_rotation_up` | The up object's world rotation applied to `world_up_vector`. |
| `vector` | `world_up_vector` in world space. |
| `none` | Use the shortest rotation from the owner's aim direction to the target direction. |

## Timeline and ownership

- **Owner.** Position owns the object's position. Aim owns its rotation.
- **Load failure.** A timeline or property animator writing a claimed
  field fails load. Constraint chains are refused, a stricter rule than
  Unity's. A constraint's inputs and owner ancestors also refuse property-driven
  transforms. <sup>[[148]](../../Citations.md#unity-constraints)</sup>
- **Sampled inputs.** The app can place sources and up objects, and the
  timeline can sample their transforms. Its float curves use `weight` and
  `weight0` through `weightN-1`, where N = `SCENE_CONSTRAINT_SOURCES_MAX`.
