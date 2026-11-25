# Position and aim constraints

The scene row wires each constraint to its sources. Timeline curves sample
weights at the current time, so seek and playback give the same pose.
Cross two source weights to transfer influence between them.

## Position constraint

The position constraint blends the owner's position toward the weighted
average of its sources. Each source contributes its world position plus
the translation offset in its binding frame. Source weights are divided
by their sum. A zero sum leaves the owner's position in place.
<sup>[[144]](../../Citations.md#unity-position-constraint)</sup>
<sup>[[148]](../../Citations.md#unity-constraints)</sup>

| Field | Meaning |
|---|---|
| `weight` | Influence from 0 to 1. Defaults to 1. Zero gives the owner's pose. <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `sources` | Ordered sources up to `SCENE_CONSTRAINT_SOURCES_MAX`. <sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `sources[].target` | Name of a scene object. <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `sources[].weight` | Relative influence from 0 to 1. Defaults to 1. <sup>[[148]](../../Citations.md#unity-constraints)</sup> |
| `translation_offset` | Offset from each source in its binding frame. Defaults to zero. <sup>[[144]](../../Citations.md#unity-position-constraint)</sup> |
| `binding_mode` | `world_space` (default), `lock_to_target` for target rotation, or `lock_to_target_with_world_up` for target yaw. <sup>[[149]](../../Citations.md#unity-cinemachine-binding)</sup> |

Lag and damping are omitted because Cinemachine Position Damping depends on
the previous frame and would make seek differ from playback.
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
| `aim_vector` | Local aim axis. Defaults to authored `[0, 0, -1]`, engine +Z. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `up_vector` | Local up axis. Defaults to `[0, 1, 0]`. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |
| `world_up_type` | How to choose the world up reference, described below. <sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `world_up_vector` | Reference vector for `vector` and `object_rotation_up`. Defaults to `[0, 1, 0]`. <sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `world_up_object` | Required scene object for `object_up` and `object_rotation_up`. <sup>[[147]](../../Citations.md#unity-aim-world-up-type)</sup> |
| `rotation_offset` | Euler degrees in the same order as `rotation`. Defaults to zero. <sup>[[146]](../../Citations.md#unity-aim-constraint)</sup> |

| World up type | Reference |
|---|---|
| `scene_up` (default) | +Y. |
| `object_up` | Direction from the constrained position to the up object. |
| `object_rotation_up` | The up object's world rotation applied to `world_up_vector`. |
| `vector` | `world_up_vector` in world space. |
| `none` | Use the shortest rotation from the owner's aim direction to the target direction. |

## Timeline and ownership

Position owns the object's position and aim owns its rotation. A timeline
or property animator writing a claimed field fails load. Source and up
object paths can be placed by the app and sampled by the timeline.
Property-driven or constrained transforms on those paths fail load.

See [Scene Files](Scene-Files.md#constraint-and-hinge) for scene rows and
track names.
