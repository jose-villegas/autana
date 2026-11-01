# Math: vectors, rotations, matrices, transforms, signals

The [math package](../../launcher/packages/math/README.md) holds scalar operations
in `scalar/`, vectors and transforms in `linear/`, shapes and queries in
`geometry/`, order statistics in `stats/`, and single-bin detection on
sample blocks in `signal/`. [Text conversion](Text.md) lives in `text/`.

<!-- generated: math-fast-links-readme check: python launcher/tools/perf/fast_paths.py --check -->
[Fast-path evidence](../packages/Fast-Paths.md) pairs costs, bounds and callers.
<!-- /generated: math-fast-links-readme -->

The arithmetic and geometry headers are:

- header-only and static inline, with no ESP-IDF, so a host suite checks every line;
- each family written once as a template ([Templates](Conventions.md#templates)) and instantiated per number type;
- a type named for its family plus a number-type suffix (`vec3f`, `quatx`), with no bare name.

Macros generate the functions, so a search for `vec3f_add` finds no definition.
Look the plain name up on a page in the [pages table](#pages); read `*_template.h` only to change an algorithm.

## Number types

| Suffix | Number type | Range | Precision | Families | For |
|---|---|---|---|---|---|
| `f` | float, single precision | about ±3.4e38 | 24-bit mantissa, ~7 digits | `vec2f`, `vec3f`, `quatf`, `affine2f`, `mat3f`, `mat4f`, `transformf`, `rectf`, `planef` | the default |
| `i` | int32 | ±2.1e9 | exact; sums and products wrap | `vec2i`, `vec3i`, `recti` | grid, pixel and cell coordinates |
| `s` | int16 | -32768 to 32767 | exact; sums and products saturate | `vec2s`, `vec3s`, `rects` | coordinates kept compact where memory is short |
| `u` | uint32 | 0 to 4.29e9 | exact; text only (`mathu`), no vector families | | counts and byte values read from text |
| `x` | Q16.16 fixed point | ±32768 | 1/65536 (1.5e-5); sine good to ~1e-3; saturates | `vec2x`, `vec3x`, `quatx`, `affine2x`, `mat4x`, `transformx`, `rectx`, `planex` | deterministic maths, or a chip with no FPU |

| Rule | Applies to |
|---|---|
| A dot product widens to int64 | `i`, `s` |
| A dot product is a saturating Q16.16 sum | `x` |
| `mat4x_apply` floors each product and wraps the sum (`mathx_dot3c`): a multiply and a shift a term, for coordinates inside ±32768 | `x` |
| `mathx_dot3_narrow` is three 32-bit products and a shift, for a caller that proves the sum fits an int32 | `x` |
| Divide by zero saturates by the numerator's sign; 0 / 0 is 0 | `x` |
| Angles are turns, `MATHX_ONE` to a turn (an eighth is `MATHX_ONE / 8`) | `x` |
| Angles are radians (`MATH_PI`, `MATH_TAU`) | `f` |

## Pages

| Page | Holds |
|---|---|
| [Conventions](Conventions.md) | Axes, composition, templates and float build rules |
| [Scalar](Scalar.md) | Arithmetic, fixed point, random draws, trigonometry and units |
| [Linear](Linear.md) | Vectors, conversions, rotations, matrices, transforms and examples |
| [Projection](Projection.md) | Near clipping and camera-to-pixel mapping |
| [Geometry](Geometry.md) | Rectangles, planes and frustum queries |
| [Stats](Stats.md) | Percentiles and medians of sample arrays |
| [Text](Text.md) | Canonical format and parse pairs, round trips and buffer capacities |
| [Signal](Signal.md) | Goertzel single-bin detection |
| [Benchmark](Benchmark.md) | Board cycles, retired instructions and QEMU counts |
