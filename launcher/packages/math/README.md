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
toolchain and the build config; the style audit's PACKAGE-INCLUDE rule
(`scripts/gates/check_style_audit.py`) fails any include that reaches the
firmware or another package.

## Using it

| Build | How it gets math |
|---|---|
| Firmware (ESP-IDF) | `launcher/packages/` is in `EXTRA_COMPONENT_DIRS`; a component says `REQUIRES math` |
| Plain CMake (the editor) | `add_subdirectory()` of this folder, then link the `math` target |
| Host scripts (run_tests.sh, render harness, reports) | `launcher/tools/build/packages.sh`: `package_includes`, `package_sources`, `package_suites` |
| Python tools | `launcher/tools/build/packages.py`: `include_roots()` |

## API

`python launcher/tools/gen/shared_helpers.py` regenerates this table from each
header's opening comment and declarations; `--check` verifies it.

<!-- generated: package-api check: python launcher/tools/gen/shared_helpers.py --check -->
| Header | Purpose | Public names |
|---|---|---|
| [include/math/linear/mat4_template.h](include/math/linear/mat4_template.h) | mat4_template: a 4x4 matrix indexed m[row][col], acting on column vectors, for one number type; see math_template.h for the macro arguments. | `MATH_DEFINE_MAT4, MATH_DEFINE_MAT4_INVERT_AFFINE, P##_apply, P##_from_trs, P##_identity, P##_invert_affine, P##_mul, P##_mul_affine, P##_row_dot_column3, P##_t` |
| [include/math/linear/mat4f.h](include/math/linear/mat4f.h) | mat4f: a 4x4 matrix of a float. | `` |
| [include/math/linear/mat4x.h](include/math/linear/mat4x.h) | mat4x: a 4x4 matrix of a Q16.16. | `` |
| [include/math/linear/math_const.h](include/math/linear/math_const.h) | math_const: the circle constants float maths uses, in radians, and the natural log of 2, which turns a half-life into a rate. | `MATH_LN2, MATH_PI, MATH_TAU` |
| [include/math/linear/math_template.h](include/math/linear/math_template.h) | math_template: how math/linear/ writes one algorithm once for several number types. | `` |
| [include/math/linear/quat_template.h](include/math/linear/quat_template.h) | quat_template: a rotation as x, y, z, w, unit length, Hamilton product, acting on a vector as q * v * q^-1, for one number type; see math_template.h for the macro arguments. | `MATH_DEFINE_QUAT, MATH_DEFINE_QUAT_NORMALIZE, P##_from_axis_angle, P##_from_basis, P##_from_euler, P##_identity, P##_mul, P##_normalize, P##_rotate, P##_t` |
| [include/math/linear/quatf.h](include/math/linear/quatf.h) | quatf: a float rotation, plus the one operation only floats have, slerp. | `quatf_slerp` |
| [include/math/linear/quatx.h](include/math/linear/quatx.h) | quatx: a Q16.16 rotation; angles are turns (mathx.h). | `` |
| [include/math/linear/transform_template.h](include/math/linear/transform_template.h) | transform_template: where a thing is, as a position, a rotation and a scale, for one number type; see math_template.h for the macro arguments. | `MATH_DEFINE_TRANSFORM, P##_compute_matrix, P##_look_along, P##_look_at, P##_looking, P##_matrix, P##_rotate, P##_set_position, P##_set_rotation, P##_set_scale, P##_t, P##_translate, P##_view` |
| [include/math/linear/transformf.h](include/math/linear/transformf.h) | transformf: a transform of a float: position, rotation, scale and the cached model matrix. | `TRANSFORMF_IDENTITY, transformf_rotate_around` |
| [include/math/linear/transformx.h](include/math/linear/transformx.h) | transformx: a transform of a Q16.16: position, rotation, scale and the cached model matrix. | `TRANSFORMX_IDENTITY` |
| [include/math/linear/vec2_template.h](include/math/linear/vec2_template.h) | vec2_template: the two-component vector for one number type; see math_template.h for the macro arguments. | `MATH_DEFINE_VEC2, P##_add, P##_dot, P##_equal, P##_scale, P##_sub, P##_t` |
| [include/math/linear/vec2f.h](include/math/linear/vec2f.h) | vec2f: a 2-component vector of a float. | `` |
| [include/math/linear/vec2i.h](include/math/linear/vec2i.h) | vec2i: a 2-component vector of an int32. | `` |
| [include/math/linear/vec2s.h](include/math/linear/vec2s.h) | vec2s: a 2-component vector of an int16. | `` |
| [include/math/linear/vec2x.h](include/math/linear/vec2x.h) | vec2x: a 2-component vector of a Q16.16. | `` |
| [include/math/linear/vec3_template.h](include/math/linear/vec3_template.h) | vec3_template: the three-component vector for one number type; see math_template.h for the macro arguments. | `MATH_DEFINE_VEC3, MATH_DEFINE_VEC3_NORMALIZE, P##_add, P##_cross, P##_dot, P##_equal, P##_normalize, P##_scale, P##_sub, P##_t` |
| [include/math/linear/vec3f.h](include/math/linear/vec3f.h) | vec3f: a 3-component vector of a float. | `vec3f_from_octahedral, vec3f_octahedral` |
| [include/math/linear/vec3i.h](include/math/linear/vec3i.h) | vec3i: a 3-component vector of an int32. | `` |
| [include/math/linear/vec3s.h](include/math/linear/vec3s.h) | vec3s: a 3-component vector of an int16. | `` |
| [include/math/linear/vec3x.h](include/math/linear/vec3x.h) | vec3x: a 3-component vector of a Q16.16. | `` |
| [include/math/linear/vec_convert.h](include/math/linear/vec_convert.h) | vec_convert: moving a vector between number types, always by an explicit call named for its destination and source (vec3f_from_vec3x, vec3s_from_vec3f, ...), never an implicit cast. | `MATH_FLOAT_INT_LIMIT, mathf_round_i32, mathf_round_s, mathf_to_x, mathx_to_f, vec2f_from_vec2i, vec2f_from_vec2s, vec2f_from_vec2x, vec2i_from_vec2f, vec2i_from_vec2s, vec2s_from_vec2f, vec2s_from_vec2i, vec2x_from_vec2f, vec3f_from_vec3i, vec3f_from_vec3s, vec3f_from_vec3x, vec3i_from_vec3f, vec3i_from_vec3s, vec3s_from_vec3f, vec3s_from_vec3i, vec3x_from_vec3f` |
| [include/math/linear/vec_swizzle_template.h](include/math/linear/vec_swizzle_template.h) | vec_swizzle_template: swizzles, a new vector made of named components of another (vec3f_zyx, vec2f_yx), for every number type. | `MATH_DEFINE_SWIZZLE2, MATH_DEFINE_SWIZZLE3, MATH_DEFINE_VEC_SWIZZLE, MATH_SWIZZLE2, MATH_SWIZZLE3, MATH_SWIZZLE_1OF2, MATH_SWIZZLE_1OF3, MATH_SWIZZLE_2OF2, MATH_SWIZZLE_2OF3, MATH_SWIZZLE_3OF2, MATH_SWIZZLE_3OF3, MATH_SWIZZLE_EACH2, MATH_SWIZZLE_EACH2_B, MATH_SWIZZLE_EACH3, MATH_SWIZZLE_EACH3_B, MATH_SWIZZLE_EACH3_C, P##_from_xy, S##_##a##b, S##_##a##b##c` |
| [include/math/motion/orbit_motion.h](include/math/motion/orbit_motion.h) | orbit_motion: moves a transform around a target, turntable style: it turns about the world's up axis (yaw) and tilts above or below the target (pitch), always facing it. | `ORBIT_MOTION_DRAG_VELOCITY_HALF_LIFE_S, ORBIT_MOTION_FLING_HALF_LIFE_S, ORBIT_MOTION_FRAME_PADDING, ORBIT_MOTION_PITCH_LIMIT, ORBIT_MOTION_RESET_SETTLED, ORBIT_MOTION_RESET_TIME_S, orbit_motion_fit_distance, orbit_motion_init, orbit_motion_input_t, orbit_motion_limits_t, orbit_motion_pose, orbit_motion_state_t, orbit_motion_t, orbit_motion_update` |
| [include/math/motion/spring_line.h](include/math/motion/spring_line.h) | spring_line, a row of points on springs: each is pulled toward rest and toward its neighbours, so a push travels along the row as a wave and dies away. | `SPRING_LINE_MAX_OFFSET, SPRING_LINE_MAX_TICKS, SPRING_LINE_OUT_ONE, SPRING_LINE_OUT_SHIFT, SPRING_LINE_REST_OFFSET, SPRING_LINE_REST_VELOCITY, SPRING_LINE_TICK_MS, spring_line_add_bump, spring_line_advance, spring_line_apply, spring_line_at_rest, spring_line_init, spring_line_nudge, spring_line_poke, spring_line_quiet, spring_line_scale, spring_line_t, spring_line_tick, spring_line_trim, spring_line_wake` |
| [include/math/scalar/fixed.h](include/math/scalar/fixed.h) | fixed: shift-based fixed-point arithmetic at caller-selected scales. | `fx_div_round, fx_div_round_wide, fx_mul_floor, fx_mul_round, fx_round_shift, fx_round_shift32` |
| [include/math/scalar/mathf.h](include/math/scalar/mathf.h) | mathf: single-precision float's scalar operations for the math templates (math_template.h). | `mathf_add, mathf_clamp, mathf_div, mathf_dot2c, mathf_dot3c, mathf_four, mathf_half_cos, mathf_half_sin, mathf_mul, mathf_neg, mathf_one, mathf_recip, mathf_sqrt, mathf_sub, mathf_two, mathf_wadd, mathf_wmul, mathf_zero` |
| [include/math/scalar/mathi.h](include/math/scalar/mathi.h) | mathi: plain integer scalar operations, for grid, pixel and cell coordinates. | `mathi_abs, mathi_add, mathi_clamp, mathi_even_ceil, mathi_even_floor, mathi_isqrt64, mathi_len, mathi_max, mathi_min, mathi_mul, mathi_neg, mathi_one, mathi_sign, mathi_size_ceil, mathi_sub, mathi_wadd, mathi_wmul, mathi_zero` |
| [include/math/scalar/maths.h](include/math/scalar/maths.h) | maths: int16 scalar operations for the math templates (math_template.h), coordinates kept compact where memory is short. | `maths_add, maths_mul, maths_neg, maths_one, maths_saturate, maths_sub, maths_wadd, maths_wmul, maths_zero` |
| [include/math/scalar/mathx.h](include/math/scalar/mathx.h) | mathx: Q16.16 fixed-point scalar operations for the math templates (math_template.h). | `MATHX_ONE, MATHX_SHIFT, mathx_add, mathx_cos_turns, mathx_div, mathx_dot2c, mathx_dot3_narrow, mathx_dot3c, mathx_four, mathx_half_cos, mathx_half_sin, mathx_mul, mathx_neg, mathx_one, mathx_saturate, mathx_sin_turns, mathx_sqrt, mathx_sub, mathx_two, mathx_wadd, mathx_wmul, mathx_zero` |
| [include/math/scalar/rng.h](include/math/scalar/rng.h) | rng: a small deterministic pseudo-random generator, for anything that wants one. | `rng_avalanche32, rng_below, rng_chance, rng_hash, rng_next, rng_seed, rng_t` |
| [include/math/scalar/trig.h](include/math/scalar/trig.h) | trig, sine and cosine of a 16-bit phase, in integers: TRIG_TURN is one turn and the result has TRIG_SIN_SHIFT fractional bits, at most TRIG_SIN_MAX. | `TRIG_QUARTER_TURN, TRIG_SIN_MAX, TRIG_SIN_SHIFT, TRIG_TURN, TRIG_TURN_SHIFT, trig_cos, trig_sin, trig_sin_quadrant` |
<!-- /generated: package-api -->
