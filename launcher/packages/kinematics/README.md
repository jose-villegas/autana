# kinematics

State moved by prescribed inputs: it follows a target, a curve or a steer it
is given, solved in closed form, with no mass and no forces. It uses one
package, [math](../math/README.md) (`set(kinematics_requires math)` in
`CMakeLists.txt`).

| Folder | Holds |
|---|---|
| `include/kinematics/follow/` | Exact steps toward a target: halving decay, a coast and a critically damped spring; [follow reference](../../../docs/kinematics/Follow.md) |
| `include/kinematics/orbit/` | A transform orbiting a target: steering, fling and reset; [orbit reference](../../../docs/kinematics/Orbit.md) |
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
| [include/kinematics/follow/follow.h](include/kinematics/follow/follow.h) | follow: a value moved toward a chosen target in closed form, so each step is solved exactly for its dt and the motion is the same at any frame rate. |
| [include/kinematics/orbit/orbit_motion.h](include/kinematics/orbit/orbit_motion.h) | orbit_motion: moves a transform around a target, turntable style: it turns about the world's up axis (yaw) and tilts above or below the target (pitch), always facing it. |
<!-- /generated: package-kinematics-headers -->
