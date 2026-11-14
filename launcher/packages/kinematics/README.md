# kinematics

State moved by prescribed inputs: it follows a target, a curve or a steer it
is given, solved in closed form, with no mass and no forces. It uses one
package, [math](../math/README.md) (`set(kinematics_requires math)` in
`CMakeLists.txt`).

| Folder | Holds |
|---|---|
| `include/kinematics/follow/` | Exact steps toward a target: halving decay, a coast and a critically damped spring; [follow reference](../../../docs/kinematics/Follow.md) |
| `include/kinematics/orbit/` | A transform orbiting a target: steering, fling and reset; [orbit reference](../../../docs/kinematics/Orbit.md) |
| `include/kinematics/fly/` | A transform flown freely from keys, a drag and a wheel; [fly reference](../../../docs/kinematics/Fly.md) |
| `include/kinematics/profile/` | A move along a line in time: rest, ease up, cruise, ease down; [profile reference](../../../docs/kinematics/Profile.md) |
| `src/` | The functions that are not inline |
| `tests/` | Its suites, run by both runners like every other suite |

Every header is spelled from `include/`, as `#include "kinematics/<part>/<name>.h"`.
Firmware declares `REQUIRES kinematics`; plain CMake builds link the `kinematics`
target. Host scripts discover sources and suites through
`launcher/tools/build/packages.sh`. The pages are indexed in
[docs/kinematics/README.md](../../../docs/kinematics/README.md).

## Headers

<!-- generated: package-kinematics-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/kinematics/fly/fly_motion.h](include/kinematics/fly/fly_motion.h) | fly_motion: a transform flown freely, as a free camera is: keys move it along its own right and forward and the world's up at its speed, a drag turns it with a level horizon (yaw about the world's up, pitch short of straight up or down, no roll), and a wheel scales its speed. |
| [include/kinematics/follow/follow.h](include/kinematics/follow/follow.h) | follow: a value moved toward a chosen target in closed form, so each step is solved exactly for its dt and the motion is the same at any frame rate. |
| [include/kinematics/orbit/orbit_motion.h](include/kinematics/orbit/orbit_motion.h) | orbit_motion: moves a transform around a target, turntable style: it turns about the world's up axis (yaw) and tilts above or below the target (pitch), always facing it. |
| [include/kinematics/profile/motion_profile.h](include/kinematics/profile/motion_profile.h) | motion_profile: a move along a line as a function of time, an S-curve motion profile. |
<!-- /generated: package-kinematics-headers -->
