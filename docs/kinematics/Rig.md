# Rig

The functions are stateless: each call takes the target pose it should follow.
A caller that wants a trailing camera passes the target's pose from an earlier
time. [Kinematics index](README.md).

![Target and follower paths in 3D, top and side views](../images/kinematics/rig-flight.png)

The target banks and climbs. Only `RIG_BIND_TARGET` follows the roll.
Open circles mark the starts.

## Follow position

`rig_follow_position` adds a target-relative offset along the bind's axes.
The Cinemachine column maps names.<sup>[[149]](../Citations.md#unity-cinemachine-binding)</sup>

| Bind | Cinemachine | Offset axes |
|---|---|---|
| `RIG_BIND_WORLD` | World Space | World axes |
| `RIG_BIND_TARGET` | Lock To Target | Target rotation, including pitch and roll |
| `RIG_BIND_TARGET_YAW` | Lock To Target With World Up | Horizontal target heading, with world +Y up |

For target position $p$, offset $o$ and the bind's rotation $R$:

```math
p_{\mathrm{follow}} = p + Ro
```

The world bind uses the identity for $R$. Target scale has no effect.

The yaw bind follows the target's forward vector projected onto the horizontal plane.<sup>[[149]](../Citations.md#unity-cinemachine-binding)</sup>
When that projection is near zero, the offset uses world axes.

## Look rotation

`rig_look_rotation` aims local `+Z` from one point to another and takes its
roll from the supplied up vector. [`quatf_looking`](../math/Linear.md#quat-looking)
defines the fallback at coincident points and poles.

## Hand-off

![Smoothstep hand-off weight over time](../images/kinematics/rig-handoff.png)

[`ease_smooth`](../math/Scalar.md#easeh) gives the incoming pose's weight over a
duration in milliseconds. Pass that weight to
[`transformf_blend`](../math/Linear.md#transform-blend).

## Example

Follow a target pose, aim at it, and transition from an outgoing camera pose.

<!-- generated: kinematics-rig-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Follow a sampled target pose, aim back at it, and ease in from the outgoing camera. */
#include "kinematics/rig/rig.h"
#include "math/scalar/ease.h"

transformf_t
rig_example(const transformf_t* outgoing, transformf_t target, uint32_t elapsed_ms, uint32_t over_ms) {
    const vec3f_t offset = {0.0F, 2.0F, -5.0F};
    transformf_t camera = transformf_identity();
    transformf_set_position(&camera, rig_follow_position(&target, offset, RIG_BIND_TARGET_YAW));
    transformf_set_rotation(&camera, rig_look_rotation(camera.position, target.position, vec3f_up()));
    const float weight = ease_smooth(elapsed_ms, over_ms);
    return transformf_blend(outgoing, &camera, weight);
}
```
<!-- /generated: kinematics-rig-example -->

The [figure producer](../../launcher/tools/kinematics/rig_figures.py) samples the
paths and weight through a C probe.

```sh
python launcher/tools/kinematics/rig_figures.py --cc cc --out docs/images/kinematics
```
