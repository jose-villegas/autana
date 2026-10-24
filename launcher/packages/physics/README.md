# physics

State advanced by integrating forces: each step pushes it by springs, pressure
or gravity it is given. It uses one package, [math](../math/README.md)
(`set(physics_requires math)` in `CMakeLists.txt`).

| Folder | Holds |
|---|---|
| `include/physics/spring/` | Integer springs on a fixed tick: a row of points carrying a wave; [spring reference](../../../docs/physics/Spring.md) |
| `include/physics/fluid/` | Water in a closed box, FLIP particles on a MAC grid; [fluid reference](../../../docs/physics/Fluid.md) |
| `src/` | The functions that are not inline |
| `tests/` | Its suites, run by both runners like every other suite |

Every header is spelled from `include/`, as `#include "physics/<part>/<name>.h"`.
Firmware declares `REQUIRES physics`; plain CMake builds link the `physics`
target. Host scripts discover sources and suites through
`launcher/tools/build/packages.sh`. The pages are indexed in
[docs/physics/README.md](../../../docs/physics/README.md).

## Headers

<!-- generated: package-physics-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/physics/fluid/flip3.h](include/physics/fluid/flip3.h) | flip3: water in a closed box, simulated as FLIP particles on a MAC grid (Harlow and Welch; Brackbill and Ruppel), blended with PIC as in Bridson, with the drift push of Muller's Ten Minute Physics FLIP tank, in 3-D. |
| [include/physics/spring/spring_line.h](include/physics/spring/spring_line.h) | spring_line, a row of points on springs: each is pulled toward rest and toward its neighbours, so a push travels along the row as a wave and dies away. |
<!-- /generated: package-physics-headers -->
