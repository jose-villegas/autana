# Hinge

A joint's local pose from a prescribed angle, with no forces or time state.
[Kinematics index](README.md).

![A plate turning about its z axis, with the rest pose and both angular limits drawn in 3D](../images/kinematics/hinge-sweep.png)

## hinge.h

| Type | Holds |
|---|---|
| `hinge_t` | The joint's local `rest` transform, unit `axis` in that rest frame, and `min` / `max` angles in radians (`min <= max`) |

| Function | Purpose |
|---|---|
| `hinge_pose` | Returns rest followed by a right-handed turn about the axis, clamping the supplied angle to the limits; leaves the hinge unchanged |

The turn is in the joint's rest frame. Its position and scale stay fixed;
its rotation composes on the right:

```math
\theta_c = \min(\theta_{\max},\max(\theta_{\min},\theta)), \qquad
q(\theta) = q_{\mathrm{rest}}\,q_{\mathrm{axis}}(\theta_c)
```

Zero gives the rest pose when the limits contain zero. Opposite axes on
mirrored joints let one prescribed angle raise both far sides together.

## Example

A plate turns about its local z axis while its pivot stays one unit up.
Angles outside its asymmetric stops give the nearest limit's pose.

<!-- generated: kinematics-hinge-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Pose a plate about its rest-frame z axis, within asymmetric stops. */
#include "kinematics/hinge/hinge.h"

#define PLATE_MIN_ANGLE (-0.2F)
#define PLATE_MAX_ANGLE 1.0F

transformf_t
hinge_example(float angle) {
    hinge_t plate = {
        .rest = transformf_identity(), .axis = vec3f_forward(), .min = PLATE_MIN_ANGLE, .max = PLATE_MAX_ANGLE};
    transformf_set_position(&plate.rest, vec3f_up());
    return hinge_pose(&plate, angle);
}
```
<!-- /generated: kinematics-hinge-example -->

The [figure producer](../../launcher/tools/kinematics/hinge_figures.py)
samples `hinge_pose` through a C probe:

```sh
python launcher/tools/kinematics/hinge_figures.py --cc cc --out docs/images/kinematics
```
