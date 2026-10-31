# math

The engine's maths library: scalar operations, linear algebra, shapes and
signal processing. It depends on nothing but the C library and libm, so every
layer of the firmware, the host tests and the tools build it. The API
covers what its types imply rather than only what today's callers use, so it may
carry functions nothing calls yet.

| Folder | Holds |
|---|---|
| `include/math/scalar/` | Scalar operations per number type (`f` float, `i` int, `s` int16, `x` Q16.16), shift-based fixed point, trig tables, a random generator |
| `include/math/linear/` | Vectors, quaternions, 4x4 matrices and transforms, each one template instantiated per number type; [linear reference](../../../docs/math/Linear.md) describes the families |
| `include/math/geometry/` | Shapes and their queries: half-open rectangles (`rectf`, `recti`, `rects`, `rectx`) and oriented planes (`planef`, `planex`), instantiated from geometry templates |
| `include/math/stats/` | Order statistics of sample arrays: percentile and median; [stats reference](../../../docs/math/Stats.md) |
| `include/math/signal/` | Signal processing on sample blocks: Goertzel single-bin detection; [signal reference](../../../docs/math/Signal.md) |
| `include/math/text/` | Canonical format and parse pairs; [text reference](../../../docs/math/Text.md) |
| `src/` | The few functions that are not inline |
| `bench/` | Shared host smoke, ESP-IDF hardware timing and QEMU instruction workloads |
| `test_apps/` | [Minimal ESP-IDF test and benchmark app](test_apps/README.md) |
| `tests/` | Unity suites for the standalone package runner and the engine host/device runners |

Library headers use `#include "math/<part>/<name>.h"`. Library code includes
only itself, the C library and the build config; PACKAGE-INCLUDE in
`scripts/gates/check_style_audit.py` checks this boundary.

`launcher/test/check_packages_alone.sh` compiles each header and source in
isolation. Test and benchmark sources also receive testkit and Unity headers.

The PACKAGE-CONFIG rule keeps first-party settings with their declaring
package and requires `MATH_` names for Kconfig entries. ESP-IDF platform
settings are allowed; comments are ignored. `CONFIG_MATH_FPU_RECIP` enables the ESP32-S3 reciprocal and
reciprocal-square-root seeds by default; the QEMU defaults disable it.

## Using it

| Build | How it gets math |
|---|---|
| Firmware (ESP-IDF) | `launcher/packages/` is in `EXTRA_COMPONENT_DIRS`; a component says `REQUIRES math` |
| Plain CMake | `add_subdirectory()` of this folder, then link the `math` target |
| Host scripts (run_tests.sh, render harness, reports) | `launcher/tools/build/packages.sh`: `package_includes`, `package_sources`, `package_suites` |
| Standalone tests | `cmake -S launcher/packages/math -B build/math && cmake --build build/math && ctest --test-dir build/math --output-on-failure` |
| Python tools | `launcher/tools/build/packages.py`: `include_roots()` |

## Headers

Every header and what it is for; the functions each family offers are in
[docs/math/README.md](../../../docs/math/README.md).
`python launcher/tools/gen/package_headers.py` regenerates this table from each
header's opening comment; `--check` verifies it.

<!-- generated: package-math-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/math/geometry/ball_template.h](include/math/geometry/ball_template.h) | ball_template: closed balls in any dimension for one number type; see docs/math/Conventions.md#templates for macro arguments. |
| [include/math/geometry/box3_plane_template.h](include/math/geometry/box3_plane_template.h) | box3_plane_template: how far a real box reaches along a plane: its least and greatest signed distance, and its least objective inside a half-space. |
| [include/math/geometry/box3_template.h](include/math/geometry/box3_template.h) | box3_template: closed axis-aligned boxes for one number type; see docs/math/Conventions.md#templates for macro arguments. |
| [include/math/geometry/box3f.h](include/math/geometry/box3f.h) | box3f: a closed axis-aligned box of a float. |
| [include/math/geometry/box3f_plane.h](include/math/geometry/box3f_plane.h) | box3f_plane: a float box against a float plane (box3_plane_template.h). |
| [include/math/geometry/box3s.h](include/math/geometry/box3s.h) | box3s: a closed axis-aligned box of int16 position ticks. |
| [include/math/geometry/circle_template.h](include/math/geometry/circle_template.h) | circle_template: closed discs for one number type; see docs/math/Conventions.md#templates for macro arguments. |
| [include/math/geometry/circlef.h](include/math/geometry/circlef.h) | circlef: a closed disc of a float. |
| [include/math/geometry/circlei.h](include/math/geometry/circlei.h) | circlei: a closed disc of an int. |
| [include/math/geometry/iso_surface.h](include/math/geometry/iso_surface.h) | iso_surface: the surface where a sampled scalar field crosses a level, as a triangle mesh, by naive surface nets (Gibson): one vertex per cell the surface passes through, at the mean of its edge crossings, and a quad across every sample edge that crosses. |
| [include/math/geometry/plane_template.h](include/math/geometry/plane_template.h) | plane_template: oriented planes for one number type; see docs/math/Conventions.md#templates for macro arguments. |
| [include/math/geometry/planef.h](include/math/geometry/planef.h) | planef: an oriented plane of a float. |
| [include/math/geometry/planex.h](include/math/geometry/planex.h) | planex: an oriented plane of a Q16.16. |
| [include/math/geometry/rect_template.h](include/math/geometry/rect_template.h) | rect_template: the half-open rectangle for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/geometry/rectf.h](include/math/geometry/rectf.h) | rectf: a half-open rectangle of a float. |
| [include/math/geometry/recti.h](include/math/geometry/recti.h) | recti: a half-open rectangle of an int. |
| [include/math/geometry/rects.h](include/math/geometry/rects.h) | rects: a half-open rectangle of an int16. |
| [include/math/geometry/rectx.h](include/math/geometry/rectx.h) | rectx: a half-open rectangle of a Q16.16. |
| [include/math/geometry/sphere_template.h](include/math/geometry/sphere_template.h) | sphere_template: closed spheres for a real number type; see docs/math/Conventions.md#templates for macro arguments. |
| [include/math/geometry/spheref.h](include/math/geometry/spheref.h) | spheref: a closed sphere of a float. |
| [include/math/linear/affine2_template.h](include/math/linear/affine2_template.h) | affine2_template: a 2D affine map for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/linear/affine2f.h](include/math/linear/affine2f.h) | affine2f: a 2D affine map of a float, with integer-pixel maps. |
| [include/math/linear/affine2x.h](include/math/linear/affine2x.h) | affine2x: a 2D affine map of a Q16.16, with integer-pixel maps. |
| [include/math/linear/mat3f.h](include/math/linear/mat3f.h) | Row-major linear matrices and their normal transforms. |
| [include/math/linear/mat4_template.h](include/math/linear/mat4_template.h) | mat4_template: a 4x4 matrix indexed m[row][col], acting on column vectors, for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/linear/mat4f.h](include/math/linear/mat4f.h) | mat4f: a 4x4 matrix of a float. |
| [include/math/linear/mat4x.h](include/math/linear/mat4x.h) | mat4x: a 4x4 matrix of a Q16.16. |
| [include/math/linear/quat_template.h](include/math/linear/quat_template.h) | quat_template: a rotation as x, y, z, w, unit length, Hamilton product, acting on a vector as q * v * q^-1, for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/linear/quatf.h](include/math/linear/quatf.h) | quatf: a float rotation with host-exact, halved-arc polynomial slerp. |
| [include/math/linear/quatx.h](include/math/linear/quatx.h) | quatx: a Q16.16 rotation; angles are turns (mathx.h). |
| [include/math/linear/transform_template.h](include/math/linear/transform_template.h) | transform_template: where a thing is, as a position, a rotation and a scale, for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/linear/transformf.h](include/math/linear/transformf.h) | transformf: a transform of a float: position, rotation, scale and the cached model matrix. |
| [include/math/linear/transformx.h](include/math/linear/transformx.h) | transformx: a transform of a Q16.16: position, rotation, scale and the cached model matrix. |
| [include/math/linear/vec2_template.h](include/math/linear/vec2_template.h) | vec2_template: the two-component vector for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/linear/vec2f.h](include/math/linear/vec2f.h) | vec2f: a 2-component vector of a float. |
| [include/math/linear/vec2i.h](include/math/linear/vec2i.h) | vec2i: a 2-component vector of an int32. |
| [include/math/linear/vec2s.h](include/math/linear/vec2s.h) | vec2s: a 2-component vector of an int16. |
| [include/math/linear/vec2x.h](include/math/linear/vec2x.h) | vec2x: a 2-component vector of a Q16.16. |
| [include/math/linear/vec3_template.h](include/math/linear/vec3_template.h) | vec3_template: the three-component vector for one number type; see docs/math/Conventions.md#templates for the macro arguments. |
| [include/math/linear/vec3f.h](include/math/linear/vec3f.h) | vec3f: a 3-component vector of a float. |
| [include/math/linear/vec3i.h](include/math/linear/vec3i.h) | vec3i: a 3-component vector of an int32. |
| [include/math/linear/vec3s.h](include/math/linear/vec3s.h) | vec3s: a 3-component vector of an int16. |
| [include/math/linear/vec3x.h](include/math/linear/vec3x.h) | vec3x: a 3-component vector of a Q16.16. |
| [include/math/linear/vec_convert.h](include/math/linear/vec_convert.h) | vec_convert: explicit vector conversions; narrowing saturates, rounding ties away from zero. |
| [include/math/linear/vec_ops_template.h](include/math/linear/vec_ops_template.h) | vec_ops_template: component-wise arithmetic and lerp for vectors of every dimension and number type. |
| [include/math/linear/vec_swizzle_template.h](include/math/linear/vec_swizzle_template.h) | vec_swizzle_template: swizzles, a new vector made of named components of another (vec3f_zyx, vec2f_yx), for every number type. |
| [include/math/projection/inverse_depth.h](include/math/projection/inverse_depth.h) | inverse_depth: depth stored as code = scale / z * code_max, for z &gt;= scale, which steps linearly across the screen. |
| [include/math/projection/project_template.h](include/math/projection/project_template.h) | project_template: shared near-plane endpoint selection for pinhole clipping. |
| [include/math/projection/projectf.h](include/math/projection/projectf.h) | projectf: pinhole projection in pixels with camera +y up and picture y down. |
| [include/math/projection/projectx.h](include/math/projection/projectx.h) | projectx: Q16.16 pinhole lens and near clipping in any fixed camera-space scale. |
| [include/math/scalar/ease.h](include/math/scalar/ease.h) | ease: timeline ramps and easing curves. |
| [include/math/scalar/fixed.h](include/math/scalar/fixed.h) | fixed: shift-based fixed-point arithmetic at caller-selected scales. |
| [include/math/scalar/float_bits.h](include/math/scalar/float_bits.h) | float_bits: bit-preserving transfers between float and uint32_t. |
| [include/math/scalar/math_const.h](include/math/scalar/math_const.h) | math_const: float circle constants, degree/radian conversion factors, and the natural log of 2, which turns a half-life into a rate. |
| [include/math/scalar/math_db.h](include/math/scalar/math_db.h) | Floored decibel conversions for nonnegative ratios. |
| [include/math/scalar/mathf.h](include/math/scalar/mathf.h) | mathf: single-precision float's scalar operations for the math templates (docs/math/Scalar.md). |
| [include/math/scalar/mathf_round.h](include/math/scalar/mathf_round.h) | mathf_round: scalar rounding to nearest, ties away from zero; finite inputs saturate to the destination type. |
| [include/math/scalar/mathi.h](include/math/scalar/mathi.h) | mathi: plain integer scalars. |
| [include/math/scalar/maths.h](include/math/scalar/maths.h) | maths: int16 scalar operations for the math templates (docs/math/Scalar.md), coordinates kept compact where memory is short. |
| [include/math/scalar/mathx.h](include/math/scalar/mathx.h) | mathx: Q16.16 fixed-point scalar operations for the math templates (docs/math/Scalar.md). |
| [include/math/scalar/rng.h](include/math/scalar/rng.h) | rng: a small deterministic pseudo-random generator, for anything that wants one. |
| [include/math/scalar/trig.h](include/math/scalar/trig.h) | trig, sine and cosine of a 16-bit phase, in integers: TRIG_TURN is one turn and the result has TRIG_SIN_SHIFT fractional bits, at most TRIG_SIN_MAX. |
| [include/math/scalar/units.h](include/math/scalar/units.h) | units: integer time conversion scales. |
| [include/math/signal/goertzel.h](include/math/signal/goertzel.h) | Streaming single-bin detection (Goertzel, 1958). |
| [include/math/signal/pitch.h](include/math/signal/pitch.h) | pitch: equal-temperament offsets relative to a caller's reference frequency. |
| [include/math/stats/stats_template.h](include/math/stats/stats_template.h) | stats_template: order statistics of a sample array for one number type; see docs/math/Conventions.md#templates for macro arguments. |
| [include/math/stats/statsf.h](include/math/stats/statsf.h) | statsf: order statistics of float samples. |
| [include/math/stats/statsi.h](include/math/stats/statsi.h) | statsi: order statistics of int32 samples, such as timings in microseconds or cycles. |
| [include/math/text/math_text.h](include/math/text/math_text.h) | math_text: canonical, allocation-free text for math values. |
| [include/math/text/text_template.h](include/math/text/text_template.h) | text_template: canonical component and geometry text over scalar OPS. |
<!-- /generated: package-math-headers -->
