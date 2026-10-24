# physics

State advanced by integrating forces: each step pushes it by springs, pressure
or gravity it is given. Motion that follows a prescribed target belongs in
kinematics; stateless maths in [math](../math/README.md), the one package this
one uses (`set(physics_requires math)` in `CMakeLists.txt`).

| Folder | Holds |
|---|---|
| `include/physics/spring/` | Integer springs on a fixed tick: a row of points carrying a wave; [spring reference](../../../docs/physics/Spring.md) |
| `tests/` | Its suites, run by both runners like every other suite |

Every header is spelled from `include/`, as `#include "physics/<part>/<name>.h"`.
Firmware declares `REQUIRES physics`; plain CMake builds link the `physics`
target. Host scripts discover sources and suites through
`launcher/tools/build/packages.sh`. The pages are indexed in
[docs/physics/README.md](../../../docs/physics/README.md).

## Headers

<!-- generated: package-physics-headers check: python launcher/tools/gen/shared_helpers.py --check -->
| Header | Purpose |
|---|---|
| [include/physics/spring/spring_line.h](include/physics/spring/spring_line.h) | spring_line, a row of points on springs: each is pulled toward rest and toward its neighbours, so a push travels along the row as a wave and dies away. |
<!-- /generated: package-physics-headers -->
