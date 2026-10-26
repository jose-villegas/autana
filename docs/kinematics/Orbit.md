# Orbit

A transform orbiting a target, with caller-owned state. [Kinematics index](README.md).

## orbit_motion.h

A transform orbits a target, with direct steering, a decaying fling and a critically damped reset.

| Function | Purpose |
|---|---|
| `orbit_motion_state_t` | Yaw and pitch in radians, and natural log of distance |
| `orbit_motion_limits_t` | Pitch and log-distance bounds |
| `orbit_motion_t` | Target, current and home states, velocity, limits and time constants |
| `orbit_motion_input_t` | One frame of steering |
| `orbit_motion_init` | At home, still, with default half-lives; clamps home to limits |
| `orbit_motion_update` | Advances by seconds; held steering cancels a reset |
| `orbit_motion_pose` | Transform with +z facing the target and +x level |
| `orbit_motion_fit_distance` | Distance that fits a sphere in the lens with padding around each half-axis |

| Constant | Purpose |
|---|---|
| `ORBIT_MOTION_PITCH_LIMIT` | Steepest pitch, short of a quarter turn |
| `ORBIT_MOTION_FLING_HALF_LIFE_S` | Time for fling speed to halve |
| `ORBIT_MOTION_DRAG_VELOCITY_HALF_LIFE_S` | Half-life for speed to follow held steering |
| `ORBIT_MOTION_RESET_TIME_S` | Reset spring time constant |
| `ORBIT_MOTION_RESET_SETTLED` | Home distance and speed threshold for settling |
| `ORBIT_MOTION_FRAME_PADDING` | Fraction of each picture half-axis left clear |

### Steering and fling

Held steering applies the frame's turn $`\Delta`$ directly. Its smoothed speed
follows that turn over the frame time $`\Delta t`$, with half-life $`h_d`$
(`ORBIT_MOTION_DRAG_VELOCITY_HALF_LIFE_S`), its share left by `follow_halved_over`:

```math
v \gets v + (\Delta/\Delta t-v)(1-2^{-\Delta t/h_d})
```

The 3D orbit shows held yaw and pitch steering, a released fling, and a reset to home.
The pose's +z faces the target; the speed strip tracks both turns.

![3D orbit with pose axes, a fading trail and pitch-limit circles through held steering, fling and reset to home; signed yaw and pitch speeds below](../images/kinematics/orbit-fling.gif)

After release, speed halves every $`h`$ seconds (`ORBIT_MOTION_FLING_HALF_LIFE_S`),
each angle stepped by `follow_coast` ([Follow](Follow.md#coast)).

Zoom stores $`\ell=\ln d`$. A zoom turn $`z`$ adds to $`\ell`$, so
$`d_{\mathrm{new}}/d_{\mathrm{old}}=e^z`$ at any starting distance.

### Reset

A reset springs each angle and the log distance home with `follow_spring`
([Follow](Follow.md#spring)), time constant `ORBIT_MOTION_RESET_TIME_S`. For yaw,
the offset is the shortest signed angle from home. It snaps home once every
offset and its speed is below `ORBIT_MOTION_RESET_SETTLED`.

### Fitting a sphere

For sphere radius $`r`$, lens half-angle $`\theta`$ and padding fraction $`p`$,
the padded cone's tangent is $`t`$:

```math
t=\tan\theta(1-p), \qquad d=\frac{r\sqrt{1+t^2}}{t}
```

The 3D view shows the camera, padded picture and sphere touching the short-side planes.
The plot compares fit distance with and without padding.

![3D padded view pyramid with the camera at its apex and a fitted sphere binding the short axis; fit distance per radius against half field of view alongside](../images/kinematics/orbit-fit-distance.png)
