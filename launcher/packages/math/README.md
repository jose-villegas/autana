# math

The engine's maths library: scalar operations per number type, vectors to
transforms over them, and motion (springs, orbiting a target). It depends on
nothing but the C library and libm, so every layer of the firmware, the host
tests, the tools and the editor build it. It may carry functions nothing calls
yet, so the API covers what the types imply rather than only what today's
callers use.

| Folder | Holds |
|---|---|
| `include/math/scalar/` | Scalar operations per number type (`f` float, `i` int, `s` int16, `x` Q16.16), shift-based fixed point, trig tables, a random generator |
| `include/math/linear/` | Vectors, quaternions, 4x4 matrices and transforms, each one template instantiated per number type; [docs/math/README.md](../../../docs/math/README.md) is the reference |
| `include/math/motion/` | Integer springs on a fixed tick, orbiting a target |
| `src/` | The few functions that are not inline |
| `tests/` | Its suites, run by both runners like every other suite |

Every header is spelled from `include/`, as `#include "math/<part>/<name>.h"`,
inside the package and out. The package includes nothing outside itself but the
C library and the build config: the style audit's PACKAGE-INCLUDE rule
(`scripts/gates/check_style_audit.py`) fails an include that reaches the
firmware or another package, and `launcher/test/check_packages_alone.sh`
compiles every header and source here with only `include/` on the path.

## Using it

| Build | How it gets math |
|---|---|
| Firmware (ESP-IDF) | `launcher/packages/` is in `EXTRA_COMPONENT_DIRS`; a component says `REQUIRES math` |
| Plain CMake (the editor) | `add_subdirectory()` of this folder, then link the `math` target |
| Host scripts (run_tests.sh, render harness, reports) | `launcher/tools/build/packages.sh`: `package_includes`, `package_sources`, `package_suites` |
| Python tools | `launcher/tools/build/packages.py`: `include_roots()` |

## Headers

Every header and what it is for; the functions each family offers are in
[docs/math/README.md](../../../docs/math/README.md).
`python launcher/tools/gen/shared_helpers.py` regenerates this table from each
header's opening comment; `--check` verifies it.

<!-- generated: package-headers check: python launcher/tools/gen/shared_helpers.py --check -->
| Header | Purpose |
|---|---|
| [include/math/linear/mat4_template.h](include/math/linear/mat4_template.h) | mat4_template: a 4x4 matrix indexed m[row][col], acting on column vectors, for one number type; see math_template.h for the macro arguments. |
| [include/math/linear/mat4f.h](include/math/linear/mat4f.h) | mat4f: a 4x4 matrix of a float. |
| [include/math/linear/mat4x.h](include/math/linear/mat4x.h) | mat4x: a 4x4 matrix of a Q16.16. |
| [include/math/linear/math_const.h](include/math/linear/math_const.h) | math_const: the circle constants float maths uses, in radians, and the natural log of 2, which turns a half-life into a rate. |
| [include/math/linear/math_template.h](include/math/linear/math_template.h) | math_template: how math/linear/ writes one algorithm once for several number types. |
| [include/math/linear/quat_template.h](include/math/linear/quat_template.h) | quat_template: a rotation as x, y, z, w, unit length, Hamilton product, acting on a vector as q * v * q^-1, for one number type; see math_template.h for the macro arguments. |
| [include/math/linear/quatf.h](include/math/linear/quatf.h) | quatf: a float rotation, plus the one operation only floats have, slerp. |
| [include/math/linear/quatx.h](include/math/linear/quatx.h) | quatx: a Q16.16 rotation; angles are turns (mathx.h). |
| [include/math/linear/transform_template.h](include/math/linear/transform_template.h) | transform_template: where a thing is, as a position, a rotation and a scale, for one number type; see math_template.h for the macro arguments. |
| [include/math/linear/transformf.h](include/math/linear/transformf.h) | transformf: a transform of a float: position, rotation, scale and the cached model matrix. |
| [include/math/linear/transformx.h](include/math/linear/transformx.h) | transformx: a transform of a Q16.16: position, rotation, scale and the cached model matrix. |
| [include/math/linear/vec2_template.h](include/math/linear/vec2_template.h) | vec2_template: the two-component vector for one number type; see math_template.h for the macro arguments. |
| [include/math/linear/vec2f.h](include/math/linear/vec2f.h) | vec2f: a 2-component vector of a float. |
| [include/math/linear/vec2i.h](include/math/linear/vec2i.h) | vec2i: a 2-component vector of an int32. |
| [include/math/linear/vec2s.h](include/math/linear/vec2s.h) | vec2s: a 2-component vector of an int16. |
| [include/math/linear/vec2x.h](include/math/linear/vec2x.h) | vec2x: a 2-component vector of a Q16.16. |
| [include/math/linear/vec3_template.h](include/math/linear/vec3_template.h) | vec3_template: the three-component vector for one number type; see math_template.h for the macro arguments. |
| [include/math/linear/vec3f.h](include/math/linear/vec3f.h) | vec3f: a 3-component vector of a float. |
| [include/math/linear/vec3i.h](include/math/linear/vec3i.h) | vec3i: a 3-component vector of an int32. |
| [include/math/linear/vec3s.h](include/math/linear/vec3s.h) | vec3s: a 3-component vector of an int16. |
| [include/math/linear/vec3x.h](include/math/linear/vec3x.h) | vec3x: a 3-component vector of a Q16.16. |
| [include/math/linear/vec_convert.h](include/math/linear/vec_convert.h) | vec_convert: moving a vector between number types, always by an explicit call named for its destination and source (vec3f_from_vec3x, vec3s_from_vec3f, ...), never an implicit cast. |
| [include/math/linear/vec_swizzle_template.h](include/math/linear/vec_swizzle_template.h) | vec_swizzle_template: swizzles, a new vector made of named components of another (vec3f_zyx, vec2f_yx), for every number type. |
| [include/math/motion/orbit_motion.h](include/math/motion/orbit_motion.h) | orbit_motion: moves a transform around a target, turntable style: it turns about the world's up axis (yaw) and tilts above or below the target (pitch), always facing it. |
| [include/math/motion/spring_line.h](include/math/motion/spring_line.h) | spring_line, a row of points on springs: each is pulled toward rest and toward its neighbours, so a push travels along the row as a wave and dies away. |
| [include/math/scalar/fixed.h](include/math/scalar/fixed.h) | fixed: shift-based fixed-point arithmetic at caller-selected scales. |
| [include/math/scalar/mathf.h](include/math/scalar/mathf.h) | mathf: single-precision float's scalar operations for the math templates (math_template.h). |
| [include/math/scalar/mathi.h](include/math/scalar/mathi.h) | mathi: plain integer scalar operations, for grid, pixel and cell coordinates. |
| [include/math/scalar/maths.h](include/math/scalar/maths.h) | maths: int16 scalar operations for the math templates (math_template.h), coordinates kept compact where memory is short. |
| [include/math/scalar/mathx.h](include/math/scalar/mathx.h) | mathx: Q16.16 fixed-point scalar operations for the math templates (math_template.h). |
| [include/math/scalar/rng.h](include/math/scalar/rng.h) | rng: a small deterministic pseudo-random generator, for anything that wants one. |
| [include/math/scalar/trig.h](include/math/scalar/trig.h) | trig, sine and cosine of a 16-bit phase, in integers: TRIG_TURN is one turn and the result has TRIG_SIN_SHIFT fractional bits, at most TRIG_SIN_MAX. |
<!-- /generated: package-headers -->
