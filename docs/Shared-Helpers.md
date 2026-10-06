# Shared helpers

Before writing a helper, search this catalogue for the operation and open its
owner. Extend that owner when the operation belongs there. The
[math reference](math/README.md) describes the concrete types instantiated
from the template declarations below.

The catalogue reads tracked headers recursively under `launcher/main/util/`:
public function declarations, static inline definitions, and function-like
macros. Template functions retain their token-paste spelling from the header.
Conditional declarations of the same name in one owner share a row.

Headers under `launcher/main/gfx/` qualify when they declare only inline
functions and macros, hold no mutable static storage, and their includes
recursively stay within qualifying gfx headers, `util/scalar/`, `util/math/`,
or flat standard-library header names. ESP and SDK configuration includes
are excluded. This selects arithmetic helpers without hardware dependencies.

Python rows are module-level public functions in `scripts/lib/*.py` and
`launcher/tools/device/*.py`; methods and nested functions are not module
helpers. Names starting with an underscore are skipped in both languages.
Each description is the first sentence of that declaration's adjacent
comment or function docstring; a module banner is not a helper description.

Regenerate from the repository root:

```sh
python launcher/tools/gen/shared_helpers.py
python launcher/tools/gen/shared_helpers.py --check
```

The check fails for a missing description or stale table. The Linux Comment
Rules workflow runs it beside the generated-document hash check. Edit the
owner's prose and regenerate rather than editing the generated block.

<!-- generated: shared-helpers sha256=ff2961dfc93e666901b49305c16eb8a9c64c87c706179caa3bfb513363059863 -->
| Helper | What it does | Owner |
|---|---|---|
| `FRAME_COST_BEGIN` | Begin a named local timing bracket when instrumentation is enabled. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `FRAME_COST_END` | Charge a timing bracket to a constant name when instrumentation is enabled. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `FRAME_COST_END_US` | FRAME_COST_END that also yields the bracket's whole elapsed microseconds, from the clock read it already makes. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `GFX_LERP_RGB888` | Interpolate three RGB888 channels as a constant expression. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `GFX_LERP_RGB888_CH` | 0xRRGGBB `t` of `n` of the way from `lo` to `hi`, per channel, truncated. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `GFX_RGB` | 0xRRGGBB to the panel's format: RGB565 with the bytes swapped, which is what this QSPI controller expects, the opposite order to the chip's native layout. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `GFX_RGB565` | 0xRRGGBB to RGB565. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `MATH_DEFINE_MAT4` | Instantiate a column-major matrix type and its operations for one scalar type. | [launcher/main/util/math/mat4_template.h](../launcher/main/util/math/mat4_template.h) |
| `MATH_DEFINE_QUAT` | Instantiate quaternion arithmetic for one scalar type. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `MATH_DEFINE_QUAT_NORMALIZE` | Instantiate quaternion normalization and rotation operations for one scalar type. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `MATH_DEFINE_SWIZZLE2` | Every two-letter swizzle of the N-component S, each returning a D. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_DEFINE_SWIZZLE3` | Every three-letter swizzle of the N-component S, each returning a D. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_DEFINE_TRANSFORM` | Instantiate a transform with a lazily cached matrix. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `MATH_DEFINE_VEC2` | Instantiate two-component vector arithmetic for one scalar type. | [launcher/main/util/math/vec2_template.h](../launcher/main/util/math/vec2_template.h) |
| `MATH_DEFINE_VEC3` | Instantiate three-component vector arithmetic for one scalar type. | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `MATH_DEFINE_VEC3_NORMALIZE` | Instantiate vector normalization and length for one scalar type. | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `MATH_DEFINE_VEC_SWIZZLE` | P is a vec3 type, V the vec2 of the same number type and T its scalar. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE2` | S_ab(v) = (v.a, v.b), a D. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE3` | S_abc(v) = (v.a, v.b, v.c), a D. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_1OF2` | Apply a macro to each of 2 component letters at nesting depth 1. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_1OF3` | Apply a macro to each of 3 component letters at nesting depth 1. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_2OF2` | Apply a macro to each of 2 component letters at nesting depth 2. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_2OF3` | Apply a macro to each of 3 component letters at nesting depth 2. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_3OF2` | Apply a macro to each of 2 component letters at nesting depth 3. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_3OF3` | Apply a macro to each of 3 component letters at nesting depth 3. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_EACH2` | F(D, S, a, b) for every pair of letters from the first N of x, y, z. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_EACH2_B` | Expand the second component of every two-letter swizzle. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_EACH3` | F(D, S, a, b, c) for every triple of letters from the first N of x, y, z. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_EACH3_B` | Expand the second component of every three-letter swizzle. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `MATH_SWIZZLE_EACH3_C` | Expand the third component of every three-letter swizzle. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `P##_add` | a + b = (ax + bx, ay + by). | [launcher/main/util/math/vec2_template.h](../launcher/main/util/math/vec2_template.h) |
| `P##_add` | a + b = (ax + bx, ay + by, az + bz). | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_apply` | M * p with w = 1, no perspective divide: for r in 0..2, p'[r] = m[r][0]*px + m[r][1]*py + m[r][2]*pz + m[r][3]. | [launcher/main/util/math/mat4_template.h](../launcher/main/util/math/mat4_template.h) |
| `P##_compute_matrix` | M = T(position) * R(rotation) * S(scale), model to parent. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_cross` | Right-handed: a x b = (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx) | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_dot` | dot = ax*bx + ay*by, summed in the wide type W so integer products cannot wrap. | [launcher/main/util/math/vec2_template.h](../launcher/main/util/math/vec2_template.h) |
| `P##_dot` | dot = ax*bx + ay*by + az*bz, summed in the wide type W so integer products cannot wrap. | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_equal` | equal = (ax == bx && ay == by), exact, no tolerance. | [launcher/main/util/math/vec2_template.h](../launcher/main/util/math/vec2_template.h) |
| `P##_equal` | equal = (ax == bx && ay == by && az == bz), exact, no tolerance. | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_from_axis_angle` | q = (axis * sin(angle/2), cos(angle/2)); `axis` must be unit length. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_from_basis` | Axes r, u, f as columns. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_from_euler` | q = qy * qx * qz, each from_axis_angle about its axis: Z, then X, then Y about the fixed axes, as Unity does. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_from_trs` | M = T * R * S, scale then rotate then translate; `rotation` must be unit length. | [launcher/main/util/math/mat4_template.h](../launcher/main/util/math/mat4_template.h) |
| `P##_from_xy` | from_xy = (xy.x, xy.y, z) | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `P##_identity` | identity: m[r][c] = (r == c ? | [launcher/main/util/math/mat4_template.h](../launcher/main/util/math/mat4_template.h) |
| `P##_identity` | identity = (0, 0, 0, 1), the rotation that does nothing. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_look_at` | f = normalize(target - position), r = normalize(up x f), u = f x r, rotation = from_basis(r, u, f). | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_matrix` | M as compute_matrix, rebuilt only while `cached` is false. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_mul` | (a * b)[r][c] = a[r][0]*b[0][c] + a[r][1]*b[1][c] + a[r][2]*b[2][c] + a[r][3]*b[3][c] | [launcher/main/util/math/mat4_template.h](../launcher/main/util/math/mat4_template.h) |
| `P##_mul` | a * b, b applied first: x = aw*bx + ax*bw + ay*bz - az*by y = aw*by - ax*bz + ay*bw + az*bx z = aw*bz + ax*by - ay*bx + az*bw w = aw*bw - ax*bx - ay*by - az*bz | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_normalize` | normalize = q * (1 / sqrt(qx*qx + qy*qy + qz*qz + qw*qw)). | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_normalize` | normalize = a * (1 / sqrt(dot(a, a))). | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_rotate` | With u = (qx, qy, qz) and t = 2 (u x v): v' = v + qw*t + u x t, which is q v q^-1 expanded for a unit `q`. | [launcher/main/util/math/quat_template.h](../launcher/main/util/math/quat_template.h) |
| `P##_rotate` | rotation = normalize(rotation * delta): about the transform's own axes, as Unity's Rotate does. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_scale` | a * s = (ax*s, ay*s), in the number type's own units (Q16.16 for fixed). | [launcher/main/util/math/vec2_template.h](../launcher/main/util/math/vec2_template.h) |
| `P##_scale` | a * s = (ax*s, ay*s, az*s), in the number type's own units (Q16.16 for fixed). | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_set_position` | position = p, cached = false; likewise rotation and scale. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_set_rotation` | Set rotation and invalidate the cached matrix. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_set_scale` | Set scale and invalidate the cached matrix. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_sub` | a - b = (ax - bx, ay - by) | [launcher/main/util/math/vec2_template.h](../launcher/main/util/math/vec2_template.h) |
| `P##_sub` | a - b = (ax - bx, ay - by, az - bz) | [launcher/main/util/math/vec3_template.h](../launcher/main/util/math/vec3_template.h) |
| `P##_translate` | position = position + delta, in the parent's frame. | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `P##_view` | V = R^T * T(-position): rotation part R^T, column 3 = -(R^T * position). | [launcher/main/util/math/transform_template.h](../launcher/main/util/math/transform_template.h) |
| `S##_##a##b` | S_ab(v) = (v.a, v.b), a D. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `S##_##a##b##c` | S_abc(v) = (v.a, v.b, v.c), a D. | [launcher/main/util/math/vec_swizzle_template.h](../launcher/main/util/math/vec_swizzle_template.h) |
| `TUNE` | Declare a bounded tunable, constant in release and registered in development. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `TUNE_GENERATION` | Read the owner generation for invalidating derived data, zero in release. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `TUNE_OWNER` | Declare the generation storage shared by one owner's tunables. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `available` | Return the sorted profile names available in the profile directory. | [launcher/tools/device/device_profile.py](../launcher/tools/device/device_profile.py) |
| `bmp_bytes_to_png` | Converts an in-memory 24bpp BMP (the exact bytes screenshot_dump() streams, see screenshot_bmp_header()'s own comment for the byte layout) into an in-memory PNG. | [launcher/tools/device/screenshot.py](../launcher/tools/device/screenshot.py) |
| `build_id` | Return the image ELF hash and build variant. | [launcher/main/util/runtime/build_id.h](../launcher/main/util/runtime/build_id.h) |
| `build_id_line` | Format an image identity as a bounded BUILD_ID console line. | [launcher/main/util/runtime/build_id.h](../launcher/main/util/runtime/build_id.h) |
| `build_id_short` | Return the abbreviated image identity for display. | [launcher/main/util/runtime/build_id.h](../launcher/main/util/runtime/build_id.h) |
| `candidates` | Find tool candidates from the environment, PATH, and installed ESP toolchains. | [scripts/lib/pinned_tool.py](../scripts/lib/pinned_tool.py) |
| `close_process_tree` | Close the Windows job, killing any descendant still in it; a no-op on POSIX. | [scripts/lib/process_tree.py](../scripts/lib/process_tree.py) |
| `create_kill_on_close_job` | Create a Windows job, or return None when ownership is unavailable. | [scripts/lib/process_tree.py](../scripts/lib/process_tree.py) |
| `div255` | v / 255, without a divide. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `expand` | 8-bit channels for 5/6/5-bit ones by bit replication, as gfx_color_rgb888() does, so rgb565() of the result gives them back. | [launcher/tools/device/gfx_color.py](../launcher/tools/device/gfx_color.py) |
| `frame_cost_add` | The slot the time went to, or NULL for a name dropped for want of one. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_append_dropped` | " +N dropped", once there is a total to trail: a name refused for want of a slot, this window. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_append_total` | The " &#124; total T.TT" tail: the sum of every slot's own average, in the same milliseconds-with-two-decimals shape as a slot's line. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_apply_pending` | Applies the waiting request, only for the owning task and only between outermost brackets, so no bracket sees two configurations. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_begin` | Begin a bracket on the shared timer and return its nesting mark. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_counters_idle` | True while nothing is armed or waiting: the counters are free for a caller that programs them itself. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_counts_line` | "perf: name cyc avg/min/max A/B/C event avg D n=N" for the armed slot, when it ran this window: the one line `autana perf` reads. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_end` | Charge a shared bracket its exclusive cost and return elapsed microseconds. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_enter` | Enter a timing bracket without hardware counter samples. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_enter_counted` | Pushes the level a bracket just started at and returns its mark, for frame_cost_leave() to read back. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_event_at` | Return the hardware event descriptor at an index, NULL outside the table. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_event_count` | Return the number of hardware event descriptors. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_event_index` | Find a hardware event by name, returning -1 when unknown. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_is_armed` | Whether a stage name matches the armed counter stage. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_leave` | Charge exclusive time and close a bracket without hardware counter samples. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_leave_counted` | Charges `name` its elapsed time minus whatever ran inside it, then folds the whole elapsed time into the level below, so that one is exclusive of it in turn. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_name_at` | The names seen so far, for a console listing; NULL past the last. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_name_index` | The index of a name the console may arm, or -1 for one never bracketed. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_names_dropped` | Return how many shared stage names could not fit in the registry. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_note_name` | Remembers a name for good, so the console can list it and check an arm against it; a full table counts the miss instead. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_post_arm` | From any task or core: asks for `name_index` (-1 disarms) with the event at `event_index`. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_report` | "name avg/worst" per slot, then a total and any drop count, then forgets the window and empties the open-bracket stack, so a report taken mid- bracket leaves no stale level for that bracket's own END to find. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_shared_name_index` | The shared instance's name index, -1 for a name never bracketed. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_shared_post_arm` | Post the counter stage and event for the frame task to apply. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_slot` | Names are string literals: one is found again by its address first. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_take_counts` | Format and consume the shared hardware counter report. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_cost_take_report` | Format and consume shared stage timings for the given frame count. | [launcher/main/util/runtime/frame_cost.h](../launcher/main/util/runtime/frame_cost.h) |
| `frame_watch_add_task` | A task whose work belongs to the frame: the panel's sender, a second core's worker. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_close_frame` | Judges the frame still open and starts the next one. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_format_json` | The last closed frame's counts and every repeating site that fits, as one JSON object. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_frames_seen` | Count the frames containing a site within the tracked window. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_json` | Format the shared watch state as bounded JSON for a host capture. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_kind_name` | Return the console label for an event kind, or ? | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_note` | A slot is free once its site has gone a whole window unseen. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_presented` | Called at each gfx_present_begin(): closes the frame and warns, once per site and interval, about every site repeating now. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_reset` | Clear the watch and begin its warm-up interval. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_restart` | frame_watch_settle() on the shared watch, for an app entered or left. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_settle` | What is drawn has changed (an app entered or left): every site's history is forgotten and the warm-up starts again. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_start` | The calling task is the one whose presents end frames. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_take_due` | True when a repeating site should be warned about now: the first time, then at most once per FRAME_WATCH_REPORT_INTERVAL_US while it keeps repeating. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_test_begin` | A self-test's watch: begin starts from nothing; end returns what the test left judged and settles, so the shell's next frames warm up again. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_test_end` | Return the self-test verdict and settle the shared watch for subsequent frames. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_verdict` | Snapshot frame count, repeating sites, and dropped events. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `frame_watch_verdict_clean` | Whether a verdict has neither repeating sites nor dropped events. | [launcher/main/util/runtime/frame_watch.h](../launcher/main/util/runtime/frame_watch.h) |
| `fx_div_round` | den must be nonzero. | [launcher/main/util/scalar/fixed.h](../launcher/main/util/scalar/fixed.h) |
| `fx_div_round_wide` | Widened quotient, nearest with ties away from zero. | [launcher/main/util/scalar/fixed.h](../launcher/main/util/scalar/fixed.h) |
| `fx_mul_floor` | Multiply two fixed-point numbers in Q(*.shift) and shift the product back down by `shift`; an arithmetic shift of a negative value floors toward negative infinity, unlike a truncating cast. | [launcher/main/util/scalar/fixed.h](../launcher/main/util/scalar/fixed.h) |
| `fx_mul_round` | Multiply two fixed-point numbers in Q(*.shift) and shift the product back down by `shift`, rounding to the nearest representable value with ties broken away from zero. | [launcher/main/util/scalar/fixed.h](../launcher/main/util/scalar/fixed.h) |
| `fx_round_shift` | Nearest integer, ties away from zero. | [launcher/main/util/scalar/fixed.h](../launcher/main/util/scalar/fixed.h) |
| `fx_round_shift32` | fx_round_shift() for a sum that already fits 32 bits, so a 32-bit core does no 64-bit arithmetic. | [launcher/main/util/scalar/fixed.h](../launcher/main/util/scalar/fixed.h) |
| `gfx_band_ring_advance` | Called once a band's send has been queued (after waiting on the previous one, if gfx_band_ring_must_wait() said so), advances the ring so the next gfx_band_ring_slot()/gfx_band_ring_row0() describe the following band. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_begin` | Reset the ring for a frame with no transfer in flight. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_done` | True once every band this frame has been handed out. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_must_wait` | True if submitting the next band's send must first wait for a previous one; false only for band 0, which has nothing in flight yet. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_row0` | The absolute row the next band starts at, given the band height in force. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_settle` | Record that the outstanding band transfer has completed. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_settled` | True once the last band handed out has also been waited for, what a frame's closing wait checks before it can skip its own wait. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_skip` | Advances past the current band WITHOUT sending it: the caller decided this band needs no redraw this frame. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_band_ring_slot` | Which of the two buffers the next render targets. | [launcher/main/gfx/gfx_band.h](../launcher/main/gfx/gfx_band.h) |
| `gfx_box_clip_segment` | Clipping to the last pixel inside preserves the rasterizer's endpoint rounding. | [launcher/main/gfx/gfx_box.h](../launcher/main/gfx/gfx_box.h) |
| `gfx_box_extend` | Union a nonempty addition into a half-open rectangle. | [launcher/main/gfx/gfx_box.h](../launcher/main/gfx/gfx_box.h) |
| `gfx_box_is_empty` | Whether either half-open rectangle dimension is empty. | [launcher/main/gfx/gfx_box.h](../launcher/main/gfx/gfx_box.h) |
| `gfx_box_outcode` | Classify a point against the four half-open clipping edges. | [launcher/main/gfx/gfx_box.h](../launcher/main/gfx/gfx_box.h) |
| `gfx_color_add` | Add `b` to `a`, saturating each channel at its own maximum: two strokes crossing on a black field make a brighter colour rather than whichever was drawn second; see boot_anim.c, where hundreds of curve segments cross and flat writes made the picture look like stacked wires. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_color_mix` | Blend packed panel colours with rounded RGB565 channels and t in 0..255. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_color_rgb565` | The panel colour for three already-quantised channels (5, 6, 5 bits), what GFX_RGB does after its own 8-bit-to-565 truncation, for a caller (a dithered tracer, say) that quantised its own channels and only needs the packing and the panel's byte swap. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_color_rgb888` | The 0xRRGGBB a packed panel colour came from: the inverse of GFX_RGB(). | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_color_swap` | Panel order to native RGB565 and back: the same byte swap both ways. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_dither_alpha_pick` | Whether the pattern covers this pixel at the given alpha. | [launcher/main/gfx/gfx_dither.h](../launcher/main/gfx/gfx_dither.h) |
| `gfx_dither_bayer_rank` | Return the recursively ordered Bayer rank at the given coordinates. | [launcher/main/gfx/gfx_dither.h](../launcher/main/gfx/gfx_dither.h) |
| `gfx_dither_covers` | Whether an ordered (Bayer) dither at `alpha` (0 nothing, 255 everything, 16 graduated levels between; see gfx_fill_rect_dither()'s own comment in gfx.c) covers absolute panel pixel (x, y). | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_dither_level` | The alpha -&gt; Bayer-level scaling gfx_dither_covers() compares against the table, exposed on its own so a caller with a whole ROW of pixels at one alpha can compute the level once instead of per pixel; see boot_anim.c's draw_image(). | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_dither_pattern` | Return pattern dimensions and ranks, NULL for an invalid identifier. | [launcher/main/gfx/gfx_dither.h](../launcher/main/gfx/gfx_dither.h) |
| `gfx_dither_threshold` | Return the pattern rank at a pixel, zero for an invalid identifier. | [launcher/main/gfx/gfx_dither.h](../launcher/main/gfx/gfx_dither.h) |
| `gfx_font_advance` | How far the cursor moves for one glyph of `ch`, at `scale`. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_glyph_run_boxes` | A 1bpp glyph's own bit-runs, coalesced across consecutive rows sharing the identical [col0, col1]: a vertical stroke becomes one box instead of one per row, before rotation, so the merge is turn-independent. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_height` | Height in pixels of one line of `f`, at `scale`. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_row_run_rect` | Screen-space rect for glyph columns [col0, col1] of one row, at `turn` (numbered as display.h) and `scale`. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_row_run_rect_dilated` | gfx_font_row_run_rect(), grown by one pixel on every side: unioning a rect's 8 unit-offset copies (ui_style.h's UI_TEXT_OUTLINED) covers the same area as this, since those 8 offsets are a full 3x3 neighbourhood minus its own centre, which the caller redraws in ink afterwards anyway. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_run_box_rect` | gfx_font_row_run_rect(), generalised from one glyph row to a row RANGE [row0, row1) sharing the same [col0, col1] run: a vertical stroke spans several rows with an identical run, and mapping the whole box at once, rather than row by row, is what turns landscape's "one narrow rect per row" into one rect regardless of turn. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_run_box_rect_dilated` | gfx_font_run_box_rect(), grown by one pixel on every side (see gfx_font_row_run_rect_dilated()'s own comment); the same Minkowski argument holds for any box, not just a single-row run. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_text_width` | Width in pixels of `len` characters of `s`, at `scale`. | [launcher/main/gfx/gfx_font.h](../launcher/main/gfx/gfx_font.h) |
| `gfx_font_ui` | The UI/body-text role. | [launcher/main/gfx/gfx_font_roles.h](../launcher/main/gfx/gfx_font_roles.h) |
| `gfx_heal_pending` | Whether any screen units are queued for healing. | [launcher/main/gfx/gfx_heal.h](../launcher/main/gfx/gfx_heal.h) |
| `gfx_heal_plan` | Picks this present's strips, at most `budget_pixels` of `width`-wide rows, and forgets the units they cover. | [launcher/main/gfx/gfx_heal.h](../launcher/main/gfx/gfx_heal.h) |
| `gfx_heal_queue_rolling` | Queues the next `rows` of a sweep down the screen, wrapping at the bottom, so a caller with no policy of its own heals everything in turn. | [launcher/main/gfx/gfx_heal.h](../launcher/main/gfx/gfx_heal.h) |
| `gfx_heal_queue_rows` | Queues every unit [y0, y1) touches, clipped to the screen. | [launcher/main/gfx/gfx_heal.h](../launcher/main/gfx/gfx_heal.h) |
| `gfx_heal_reset` | Clear pending healing and restart the lattice and rolling sweep. | [launcher/main/gfx/gfx_heal.h](../launcher/main/gfx/gfx_heal.h) |
| `gfx_heal_strip_start` | The start of the lattice strip holding `row`, before clipping to the screen; negative when `row` sits above the lattice's first full strip. | [launcher/main/gfx/gfx_heal.h](../launcher/main/gfx/gfx_heal.h) |
| `gfx_hue_rgb` | 0xRRGGBB at full saturation and full brightness. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_indexed_cell_changed` | True if a cell moving from `old_idx` to `new_idx` is worth a repaint and a send: always, by raw index, when `dither16_on` is false (256 mode, no further quantizing to exploit); by dither CLASS when it is true (16 mode); see gfx_indexed_dither16_classify()'s own comment for why that is exact, not an approximation. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_cell_dither_changed` | Exact, not an approximation: a fixed cell never leaves its own (cx, cy) phase, so comparing the two indices' resolved colours at THAT phase alone is the whole answer, unlike gfx_indexed_dither16_classify()'s every-phase match, needed only because a PIXEL mode's cell spans several. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_cell_needs_repaint` | gfx_indexed_cell_changed(), plus the one case it cannot see for itself: `force_full` set means the PANEL, not just this cell's own index, needs repainting (an overlay just closed, the board turned), so every visited cell is worth a write and a send whether or not its index moved. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_cell_repaint` | The one per-cell decision, cheap in every kind: `force_full` widens before either table is touched, and an unmoved index answers `false` before any lookup at all; guards a switch-per-cell shape once skipped. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_classify` | Groups the 256 indices by whether a `phases`-wide table renders them IDENTICALLY: a cell whose index moves within a class has provably unchanged output wherever it sits. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_dither16_classify` | Group palette indices whose entire ordered-dither output is identical. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_expand_panel_row` | Panel row `y` of `frame`, exactly as the present path sends it. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_expand_row` | Expands one panel output row from a row of palette-index bytes: pixel x reads `grid_row[x / cell_size]`, looked up in `lut`. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_expand_row_dither16` | Same as gfx_indexed_expand_row(), but every pixel is one lookup into a precomputed (index, phase) table instead of a 256-colour LUT read. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_expand_row_dither_cell` | One lookup per CELL, keyed by its own (cx, cy) phase, filled solid across its pixels by a run fill; `bayer2` picks the phase formula matching whichever table `cell_table` was baked for, resolved once per cell rather than the divide, ternary and multiply gfx_indexed_expand_row() never needs repeated once per PANEL PIXEL. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_expand_row_dither_checker2` | Same shape as gfx_indexed_expand_row_dither16(), read at a 2-pixel period instead of 4: `panel_row`/`panel_col0` are absolute panel coordinates, so two dithered bands sent side by side stay in phase. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_grid_row_to_panel_row` | Return the first panel row covered by a grid row. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_indexed_panel_row_to_grid_row` | The grid row a panel row belongs to, and the reverse: the first/last panel row a grid row occupies. | [launcher/main/gfx/gfx_indexed.h](../launcher/main/gfx/gfx_indexed.h) |
| `gfx_mode_resolve` | Resolves a request against the system's resolution cap and the panel's own geometry, without touching any buffer. | [launcher/main/gfx/gfx_mode.h](../launcher/main/gfx/gfx_mode.h) |
| `gfx_palette_index_of` | Nearest-entry index of `c` in whichever palette `index_map` was built for (tools/gen/gfx_palette_gen.h's gfx_palette_gen_build_index_map()); one flash read, keyed by native (non-byte-swapped) RGB565, the same swap gfx_color_rgb888() undoes. | [launcher/main/gfx/gfx_palette.h](../launcher/main/gfx/gfx_palette.h) |
| `gfx_rgb565_b5` | Extract the five-bit blue channel from native RGB565. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_rgb565_g6` | Extract the six-bit green channel from native RGB565. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_rgb565_r5` | The channels of a NATIVE RGB565 value (gfx_color_swap() a panel colour first): red and blue are 5 bits, green 6. | [launcher/main/gfx/gfx_color.h](../launcher/main/gfx/gfx_color.h) |
| `gfx_target_clip_y` | The absolute row range [y0, y1) intersected with `clip`'s and the target's own row range, never wider than either. | [launcher/main/gfx/gfx_target.h](../launcher/main/gfx/gfx_target.h) |
| `gfx_target_fill_rect` | gfx_fill_rect()'s own body, extracted here so the exact arithmetic every fill goes through is the one a host suite can drive directly. | [launcher/main/gfx/gfx_target.h](../launcher/main/gfx/gfx_target.h) |
| `gfx_target_row` | The row pointer for absolute row `y`, once it is known to lie inside [target.y0, target.y0 + target.height): gfx_target_clip_y() above is what a caller uses to know that. | [launcher/main/gfx/gfx_target.h](../launcher/main/gfx/gfx_target.h) |
| `gfx_target_row_range_overlaps` | True if a shape whose own row extent is [y0, y1) has anything at all to draw into this target, the check a caller decides whether to bother drawing with, before paying for the call: a per-band triangle bin and ui.c's per-band command replay both use this to skip work outside the current band rather than discover it clips to nothing. | [launcher/main/gfx/gfx_target.h](../launcher/main/gfx/gfx_target.h) |
| `help_text` | Describe supported project-local settings and their value types. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `icon_walk_blocks` | Fits an icon to a box and EMITS each run rather than collecting them, so per-draw stack is O(1) in the icon's size. | [launcher/main/gfx/icon.h](../launcher/main/gfx/icon.h) |
| `job_run_core1` | Runs a valid job on core 1 when its worker is idle; otherwise runs it inline. | [launcher/main/util/runtime/job.h](../launcher/main/util/runtime/job.h) |
| `job_try_core1` | Dispatches to core 1 or not at all: false means the caller must do the work itself. | [launcher/main/util/runtime/job.h](../launcher/main/util/runtime/job.h) |
| `job_wait` | Waits for the dispatched core-1 job. | [launcher/main/util/runtime/job.h](../launcher/main/util/runtime/job.h) |
| `json_splice_key` | False, with `json` untouched, when it does not end in `}` or the whole fragment does not fit in `json_size`: a truncated fragment would break the line for whoever parses it. | [launcher/main/util/encode/json_splice.h](../launcher/main/util/encode/json_splice.h) |
| `launch_process_tree` | Start command as one stoppable tree: a new session on POSIX, a job assigned before it runs on Windows. | [scripts/lib/process_tree.py](../scripts/lib/process_tree.py) |
| `load` | Parse a profile into a dict of str -&gt; str. | [launcher/tools/device/device_profile.py](../launcher/tools/device/device_profile.py) |
| `load` | The settings of `project` (default: the one this command acts on) as {key: value}; empty when it has no file. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `mathf_add` | Add scalar values with single-precision arithmetic. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_clamp` | Clamp a value to a closed interval. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_div` | Divide scalar values with single-precision arithmetic. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_dot2c` | a0 * b0 + a1 * b1 + c, summed left to right. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_dot3c` | a0 * b0 + a1 * b1 + a2 * b2 + c, summed left to right. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_four` | Return four in the scalar type's own units. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_half_cos` | Return cosine of half an angle in radians. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_half_sin` | Return sine of half an angle in radians. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_mul` | Multiply scalar values with single-precision arithmetic. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_neg` | Negate scalar values with single-precision arithmetic. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_one` | Return one in the scalar type's own units. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_round_i32` | Round to the nearest int32, ties away from zero, saturating at the endpoints. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `mathf_round_s` | Round scaled float units to a saturated int16 coordinate. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `mathf_sqrt` | Return the single-precision square root. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_sub` | Subtract scalar values with single-precision arithmetic. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_to_x` | Convert float units to rounded and saturated Q16.16. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `mathf_two` | Return two in the scalar type's own units. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_wadd` | Add single-precision dot-product terms. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_wmul` | Multiply single-precision dot-product terms. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathf_zero` | Return zero in the scalar type's own units. | [launcher/main/util/scalar/mathf.h](../launcher/main/util/scalar/mathf.h) |
| `mathi_abs` | Return the integer magnitude; the caller excludes INT_MIN. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_add` | Add scalar values with wrapping int32 arithmetic. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_clamp` | Clamp a value to a closed interval. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_even_ceil` | Round a coordinate up to an even panel edge. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_even_floor` | Rounds down/up to an even coordinate: the panel controller only takes a window on even edges. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_isqrt64` | Floor of the square root over the whole uint64_t range. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_len` | &#124;(x, y)&#124; without a square root, to about 4%: the larger component plus two fifths of the smaller. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_max` | Return the larger integer. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_min` | Return the smaller integer. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_mul` | Multiply scalar values with wrapping int32 arithmetic. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_neg` | Negate scalar values with wrapping int32 arithmetic. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_one` | Return one in the scalar type's own units. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_sign` | -1, 0 or 1. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_sub` | Subtract scalar values with wrapping int32 arithmetic. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_wadd` | Add dot-product terms in int64. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_wmul` | Multiply dot-product terms in int64. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `mathi_zero` | Return zero in the scalar type's own units. | [launcher/main/util/scalar/mathi.h](../launcher/main/util/scalar/mathi.h) |
| `maths_add` | Add scalar values with saturation to int16. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_mul` | Multiply scalar values with saturation to int16. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_neg` | Negate scalar values with saturation to int16. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_one` | Return one in the scalar type's own units. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_saturate` | Clamp an int32 value to the int16 range. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_sub` | Subtract scalar values with saturation to int16. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_wadd` | Add dot-product terms in int64. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_wmul` | Multiply dot-product terms in int64. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `maths_zero` | Return zero in the scalar type's own units. | [launcher/main/util/scalar/maths.h](../launcher/main/util/scalar/maths.h) |
| `mathx_add` | Add scalar values with saturation to Q16.16. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_cos_turns` | Return Q16.16 cosine for an angle measured in Q16.16 turns. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_div` | Divide Q16.16 values with rounding and saturation; zero denominators saturate by numerator sign. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_dot2c` | The dot products a matrix transform sums, as a fast path: each product is floored to Q16.16 (a multiply and a funnel shift, no 64-bit sum, no sign branch) and the 32-bit sum WRAPS, unlike mathx_add. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_dot3_narrow` | a0 * b0 + a1 * b1 + a2 * b2 + c, then an arithmetic shift right by `shift`: plain 32-bit products and sum, a multiply and an add each, for operands whose whole sum fits in an int32. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_dot3c` | Sum three floored Q16.16 products and an offset with wrapping int32 arithmetic. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_four` | Return four in the scalar type's own units. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_half_cos` | Return Q16.16 cosine of half an angle measured in turns. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_half_sin` | Return Q16.16 sine of half an angle measured in turns. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_mul` | Multiply scalar values with saturation to Q16.16, rounded to nearest with ties away from zero. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_neg` | Negate scalar values with saturation to Q16.16. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_one` | Return one in the scalar type's own units. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_saturate` | Clamp an int64 value to the int32 range. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_sin_turns` | Return Q16.16 sine for an angle measured in Q16.16 turns. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_sqrt` | The square root of a non-negative value; a negative one gives 0. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_sub` | Subtract scalar values with saturation to Q16.16. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_to_f` | Convert Q16.16 units to single-precision float. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `mathx_two` | Return two in the scalar type's own units. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_wadd` | Add Q16.16 dot-product terms with saturation. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_wmul` | Multiply Q16.16 dot-product terms with saturation. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `mathx_zero` | Return zero in the scalar type's own units. | [launcher/main/util/scalar/mathx.h](../launcher/main/util/scalar/mathx.h) |
| `memory_alloc` | A byte-addressable block of this kind, or NULL when none is that large. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `memory_caps` | Map a buffer purpose to the heap capabilities required by the allocator. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `memory_dump` | Prints every region of this kind and its blocks to the console. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `memory_free` | Returns a block from memory_alloc(); NULL is a no-op. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `memory_free_bytes` | Return the free bytes available in the requested heap kind. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `memory_largest_block` | The largest single block an allocation of this kind could still get: free space alone does not say whether one big buffer fits. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `memory_total_bytes` | Everything this kind's heap regions hold, free or not. | [launcher/main/util/runtime/memory.h](../launcher/main/util/runtime/memory.h) |
| `panel_point` | The panel pixel that turn_png(png, quarter) moved to (x, y), for a width x height panel image: turn_png's inverse, for one point. | [launcher/tools/device/screenshot.py](../launcher/tools/device/screenshot.py) |
| `parse` | Parse supported project settings, rejecting malformed or unknown keys. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `path_value` | A configured path: `~` expanded, relative to the project. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `project_dir` | Return the project selected for this command, defaulting to the working directory. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `quatf_slerp` | slerp = a sin((1 - t) O) / sin O + b sin(t O) / sin O, with cos O = a . | [launcher/main/util/math/quatf.h](../launcher/main/util/math/quatf.h) |
| `read_define` | Read a required integer definition from the panel header text. | [launcher/tools/device/panel_size.py](../launcher/tools/device/panel_size.py) |
| `read_screenshot` | Trigger one capture on an already-open connection and return (png_bytes, state_json_or_None). | [launcher/tools/device/screenshot.py](../launcher/tools/device/screenshot.py) |
| `require` | Read one field, failing loudly on missing or unmeasured values. | [launcher/tools/device/device_profile.py](../launcher/tools/device/device_profile.py) |
| `resolve` | Resolve the pinned tool major or exit with installation guidance. | [scripts/lib/pinned_tool.py](../scripts/lib/pinned_tool.py) |
| `results` | Strict Unity records in capture order, with optional ignored tests. | [scripts/lib/device_capture.py](../scripts/lib/device_capture.py) |
| `rgb565` | GFX_RGB565 from 8-bit channels: truncating, not rounding. | [launcher/tools/device/gfx_color.py](../launcher/tools/device/gfx_color.py) |
| `rng_avalanche32` | A counter-based draw: (seed, a, b, c) always hashes to the same value, in any order, on any core, what a checkerboard-parallel step needs, where two same-phase cells may be drawn by either core in either order. | [launcher/main/util/scalar/rng.h](../launcher/main/util/scalar/rng.h) |
| `rng_below` | A number in 0 .. | [launcher/main/util/scalar/rng.h](../launcher/main/util/scalar/rng.h) |
| `rng_chance` | True with probability `chance` in 256. | [launcher/main/util/scalar/rng.h](../launcher/main/util/scalar/rng.h) |
| `rng_hash` | Hash a seed and three coordinates without consuming generator state. | [launcher/main/util/scalar/rng.h](../launcher/main/util/scalar/rng.h) |
| `rng_next` | Advance xorshift32 state and return the next nonzero word. | [launcher/main/util/scalar/rng.h](../launcher/main/util/scalar/rng.h) |
| `rng_seed` | Any seed will do except zero, which is a fixed point of the algorithm: the state would stay zero and every draw would return zero for ever. | [launcher/main/util/scalar/rng.h](../launcher/main/util/scalar/rng.h) |
| `screenshot_base64_encode` | Encodes `len` bytes at `in` into `out`, which must hold at least screenshot_base64_encoded_len(len) bytes. | [launcher/main/util/encode/screenshot.h](../launcher/main/util/encode/screenshot.h) |
| `screenshot_base64_encoded_len` | How many bytes screenshot_base64_encode() writes for `len` input bytes, NOT including a NUL terminator, which callers wanting a C string must budget for separately. | [launcher/main/util/encode/screenshot.h](../launcher/main/util/encode/screenshot.h) |
| `screenshot_bmp_header` | Fills `out[SCREENSHOT_BMP_HEADER_SIZE]` with a BITMAPFILEHEADER + BITMAPINFOHEADER for an uncompressed, bottom-up, 24bpp BMP of `width` x `height` pixels; pixel data follows. | [launcher/main/util/encode/screenshot.h](../launcher/main/util/encode/screenshot.h) |
| `screenshot_bmp_row_stride` | Bytes per row once padded to BMP's 4-byte row boundary: width * 3 (24bpp, no alpha), rounded up to the next multiple of 4. | [launcher/main/util/encode/screenshot.h](../launcher/main/util/encode/screenshot.h) |
| `settings_read_i32` | True with `*out` set when the key holds a value; false when it was never written or the store is unavailable. | [launcher/main/util/runtime/settings.h](../launcher/main/util/runtime/settings.h) |
| `settings_store_start` | Starts the store: an init that reports it stale is followed by one erase and one more init. | [launcher/main/util/runtime/settings_policy.h](../launcher/main/util/runtime/settings_policy.h) |
| `settings_write_i32` | True when the value is stored and committed. | [launcher/main/util/runtime/settings.h](../launcher/main/util/runtime/settings.h) |
| `spring_line_add_bump` | Adds `amount` at column `x`, falling smoothly to nothing `half_width` columns away as (1 - t^2)^2. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_advance` | Runs the ticks `dt_ms` is worth, carrying the remainder. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_apply` | Writes rest + offset into `out`, both Q4, and reports the columns whose value changed, half open and empty when none did, measured against what `out` holds, so it has to be the array last drawn. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_at_rest` | Whether the active spring range is empty. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_init` | Initialize the line using caller-owned offset and velocity buffers. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_nudge` | A shove: the line is moved, Q16 pixels, and let go. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_poke` | A flick: velocity, Q16 pixels per tick. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_quiet` | Whether one column meets both rest thresholds. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_scale` | Scale a signed value by a fraction out of 256, truncating toward zero. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_tick` | One tick. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_trim` | Puts to rest the quiet columns at each END of the active range, which is all of them once the whole line is quiet. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `spring_line_wake` | Include a clipped half-open column range in the active springs. | [launcher/main/util/motion/spring_line.h](../launcher/main/util/motion/spring_line.h) |
| `stop_process_tree` | Stop a tree started by launch_process_tree, then release its job. | [scripts/lib/process_tree.py](../scripts/lib/process_tree.py) |
| `string` | The string starting at text[at] and the index after it. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `swap` | Panel order to native RGB565 and back: the same byte swap both ways. | [launcher/tools/device/gfx_color.py](../launcher/tools/device/gfx_color.py) |
| `terminate_tree` | Escalate POSIX termination; on Windows close the job, or taskkill the tree when no job was assigned. | [scripts/lib/process_tree.py](../scripts/lib/process_tree.py) |
| `timing_now_us` | Microseconds since boot, monotonic. | [launcher/main/util/runtime/timing.h](../launcher/main/util/runtime/timing.h) |
| `timing_sleep_ms` | Blocks the calling task for at least `ms`, letting others run. | [launcher/main/util/runtime/timing.h](../launcher/main/util/runtime/timing.h) |
| `timing_yield` | Gives up the rest of this scheduler tick: enough for the idle task to feed the watchdog, without a wait worth naming. | [launcher/main/util/runtime/timing.h](../launcher/main/util/runtime/timing.h) |
| `to_native` | Convert Git Bash drive paths to native Windows paths; other paths pass through. | [scripts/lib/native_path.py](../scripts/lib/native_path.py) |
| `tool_major` | Read a tool version major, returning None when probing fails. | [scripts/lib/pinned_tool.py](../scripts/lib/pinned_tool.py) |
| `trig_cos` | Return Q15 cosine at a uint16 phase covering one complete turn. | [launcher/main/util/scalar/trig.h](../launcher/main/util/scalar/trig.h) |
| `trig_sin` | sin of a 16-bit phase (65536 == one turn), Q15. | [launcher/main/util/scalar/trig.h](../launcher/main/util/scalar/trig.h) |
| `trig_sin_quadrant` | Interpolated, not snapped. | [launcher/main/util/scalar/trig.h](../launcher/main/util/scalar/trig.h) |
| `tune_count` | Return the number of registered tunables. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `tune_find` | Find a registered tunable by name, NULL when absent. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `tune_handle_line` | Handle a console tuning command against the shared registry. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `tune_register` | Kept in name order, so a listing does not depend on link order. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `tune_registry_handle_line` | True if `line` was one of these, whatever came of it. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `tune_shared` | The one TUNE() entries join, and the console answers from. | [launcher/main/util/runtime/tune.h](../launcher/main/util/runtime/tune.h) |
| `turn_png` | Turn an RGB PNG made by bmp_bytes_to_png() clockwise by `quarter`. | [launcher/tools/device/screenshot.py](../launcher/tools/device/screenshot.py) |
| `tween_ease_in` | Slow off the mark, fast by the end, mirror of tween_ease_out() above. | [launcher/main/util/motion/tween.h](../launcher/main/util/motion/tween.h) |
| `tween_ease_out` | Fast off the mark, settling as it arrives. | [launcher/main/util/motion/tween.h](../launcher/main/util/motion/tween.h) |
| `tween_lerp_i32` | a, at u8 = 0; b, at u8 = 255; linear between. | [launcher/main/util/motion/tween.h](../launcher/main/util/motion/tween.h) |
| `tween_ramp` | 0 before `start_ms`, 255 from `start_ms + dur_ms` on, linear between. | [launcher/main/util/motion/tween.h](../launcher/main/util/motion/tween.h) |
| `value` | A string or a one-line array of strings from text[at:], and what follows it. | [scripts/lib/autana_config.py](../scripts/lib/autana_config.py) |
| `vec2f_from_vec2i` | Convert a 2-component vector using single-precision casts. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2f_from_vec2s` | Convert a 2-component vector multiplying by the caller scale. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2f_from_vec2x` | Convert a 2-component vector converting Q16.16 to float units. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2i_from_vec2f` | Convert a 2-component vector rounding to nearest with ties away from zero and saturation. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2i_from_vec2s` | Convert a 2-component vector widening components exactly. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2s_from_vec2f` | Convert a 2-component vector dividing by the caller scale, rounding and saturating. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2s_from_vec2i` | Convert a 2-component vector saturating components to int16. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec2x_from_vec2f` | Convert a 2-component vector rounding and saturating float units to Q16.16. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3f_from_vec3i` | Convert a 3-component vector using single-precision casts. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3f_from_vec3s` | Convert a 3-component vector multiplying by the caller scale. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3f_from_vec3x` | Convert a 3-component vector converting Q16.16 to float units. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3i_from_vec3f` | Convert a 3-component vector rounding to nearest with ties away from zero and saturation. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3i_from_vec3s` | Convert a 3-component vector widening components exactly. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3s_from_vec3f` | Convert a 3-component vector dividing by the caller scale, rounding and saturating. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3s_from_vec3i` | Convert a 3-component vector saturating components to int16. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `vec3x_from_vec3f` | Convert a 3-component vector rounding and saturating float units to Q16.16. | [launcher/main/util/math/vec_convert.h](../launcher/main/util/math/vec_convert.h) |
| `windows_job_binding` | Bind Windows job-object functions with pointer-safe ctypes signatures. | [scripts/lib/process_tree.py](../scripts/lib/process_tree.py) |
| `write_capture` | png/.json beside each other, `out`'s extension replaced with .png. | [launcher/tools/device/screenshot.py](../launcher/tools/device/screenshot.py) |
<!-- /generated: shared-helpers -->
