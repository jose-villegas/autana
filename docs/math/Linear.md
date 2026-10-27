# Linear

Vectors, rotations, affine maps and cached transforms. [Math index](README.md).

<!-- generated: math-fast-links-linear check: python launcher/tools/perf/fast_paths.py --check -->
Fast-path evidence: [mathx_rsqrt_q30](Fast-Paths.md#mathx-rsqrt-q30).
<!-- /generated: math-fast-links-linear -->

`P` stands for the type's prefix (`vec3f`, `vec3x`, ...). The families pass small
structs by value, and inputs may alias outputs. Output pointers receive
transform state, affine2 inverses and normalization quotients.

## vec2 (`f i s x`)

| Function | Purpose | Formula |
|---|---|---|
| `P_min(a, b)` | component-wise min | $`\min(a_k,b_k)`$ |
| `P_max(a, b)` | component-wise max | $`\max(a_k,b_k)`$ |
| `P_clamp(v, lo, hi)` | component-wise clamp | $`\min(\max(v_k,lo_k),hi_k)`$ |
| `P_repeat(v, len)` | component-wise repeat | $`\mathrm{repeat}(v_k,len_k)`$ |
| `P_wrap(v, lo, hi)` | component-wise wrap | $`\mathrm{wrap}(v_k,lo_k,hi_k)`$ |
| `P_lerp(a, b, t)`, `P_lerp(a, b, t, n)` | per-component lerp: `f x` take one weight `t`; `i s` take `t` of `n`, truncating toward `a` | $`\mathrm{lerp}(a_k,b_k,t)`$ or $`\mathrm{lerp}(a_k,b_k,t,n)`$ |
| `P_add(a, b)` | component sum | $`a + b = (a_x + b_x,\ a_y + b_y)`$ |
| `P_sub(a, b)` | component difference | $`a - b = (a_x - b_x,\ a_y - b_y)`$ |
| `P_scale(a, s)` | every component times the scalar `s` | $`s\,a = (s\,a_x,\ s\,a_y)`$ |
| `P_dot(a, b)` | dot product, in the wide type of the number type | $`a \cdot b = a_x b_x + a_y b_y`$ |
| `P_equal(a, b)` | exact component equality | $`a_x = b_x \land a_y = b_y`$ |
| `P_<c1><c2>(v)` | swizzle, each `c` one of `x y`: `vec2f_yx(v)` swaps | $`(v_{c_1},\ v_{c_2})`$ |
| `P_<c1><c2><c3>(v)` | swizzle to the vec3 of the same number type, each `c` one of `x y` | $`(v_{c_1},\ v_{c_2},\ v_{c_3})`$ |

## vec3 (`f i s x`)

| Function | Purpose | Formula |
|---|---|---|
| `P_min(a, b)` | component-wise min | $`\min(a_k,b_k)`$ |
| `P_max(a, b)` | component-wise max | $`\max(a_k,b_k)`$ |
| `P_clamp(v, lo, hi)` | component-wise clamp | $`\min(\max(v_k,lo_k),hi_k)`$ |
| `P_repeat(v, len)` | component-wise repeat | $`\mathrm{repeat}(v_k,len_k)`$ |
| `P_wrap(v, lo, hi)` | component-wise wrap | $`\mathrm{wrap}(v_k,lo_k,hi_k)`$ |
| `P_lerp(a, b, t)`, `P_lerp(a, b, t, n)` | per-component lerp: `f x` take one weight `t`; `i s` take `t` of `n`, truncating toward `a` | $`\mathrm{lerp}(a_k,b_k,t)`$ or $`\mathrm{lerp}(a_k,b_k,t,n)`$ |
| `P_add(a, b)` | component sum | $`a + b = (a_x + b_x,\ a_y + b_y,\ a_z + b_z)`$ |
| `P_sub(a, b)` | component difference | $`a - b = (a_x - b_x,\ a_y - b_y,\ a_z - b_z)`$ |
| `P_scale(a, s)` | every component times the scalar `s` | $`s\,a = (s\,a_x,\ s\,a_y,\ s\,a_z)`$ |
| `P_dot(a, b)` | dot product, in the wide type of the number type | $`a \cdot b = a_x b_x + a_y b_y + a_z b_z`$ |
| `P_equal(a, b)` | exact component equality | $`a_x = b_x \land a_y = b_y \land a_z = b_z`$ |
| `P_cross(a, b)` | cross product; x × y = z | $`(a \times b)_x = a_y b_z - a_z b_y`$<br> $`(a \times b)_y = a_z b_x - a_x b_z`$<br> $`(a \times b)_z = a_x b_y - a_y b_x`$ |
| `P_normalize(a)` | unit vector; `f` and `x` only; `f` needs `a` nonzero | $`\hat a = \frac{a}{\sqrt{a \cdot a}}`$ |
| `P_normalize_divide(a, t, &q)` | normalize and divide a scalar by the same length; `f` and `x` only | $`\mathrm{out}=a/\lVert a\rVert,\ q=t/\lVert a\rVert`$ |
| `P_<c1><c2><c3>(v)` | swizzle, each `c` one of `x y z`: `vec3f_zyx(v)` reverses | $`(v_{c_1},\ v_{c_2},\ v_{c_3})`$ |
| `P_<c1><c2>(v)` | swizzle to the vec2 of the same number type, each `c` one of `x y z` | $`(v_{c_1},\ v_{c_2})`$ |
| `P_from_xy(xy, z)` | the vec3 of a vec2 and a `z` | $`(\mathit{xy}_x,\ \mathit{xy}_y,\ z)`$ |
| `vec3f_octahedral(n)` | the point of the square $`[-1, 1]^2`$ a direction maps to<sup>[[21]](../Citations.md#cigolle-2014)</sup>, the $`z < 0`$ half folded over the diagonals; any length but zero; `f` only | $`(u, v) = \frac{(n_x,\ n_y)}{\lvert n_x\rvert + \lvert n_y\rvert + \lvert n_z\rvert}`$<br> folded to $`(\mathrm{sign}(u)\,(1 - \lvert v\rvert),\ \mathrm{sign}(v)\,(1 - \lvert u\rvert))`$ when $`n_z < 0`$ |
| `vec3f_from_octahedral(p)` | the unit direction of a point of the square, the inverse of `vec3f_octahedral`; `f` only | $`z = 1 - \lvert p_x\rvert - \lvert p_y\rvert`$<br> the point unfolded when $`z < 0`$<br> then normalized |

Float normalization uses `mathf_rsqrt` for the reciprocal length.
The scalar limit is `MATHF_RSQRT_ULP_LIMIT`; nonzero input remains required.
The [benchmark table](Benchmark.md) reports measured component error against the `1 / sqrtf` path.

`vec3x_normalize` and `vec3x_normalize_divide` accept any nonzero int32 vector.
Components are within $`0.5 + 2^{-12}`$ LSB (nearest value or neighbour on a near-tie).
For `x`, a zero vector gives zero and `q = mathx_div(t, 0)`.

The quotient `*q` is within relative $`2^{-28}`$ plus 0.5 LSB of
$`t/\lVert a\rVert`$ clamped to the int32 range.

A swizzle copies components and does no arithmetic. One that changes dimension
returns the other vector type of the same number type: `vec3f_xz(v)` is a `vec2f_t`.

Each vec3 header instantiates `vec_swizzle_template.h` after including its vec2.

## Conversions (`vec_convert.h`)

Always an explicit call named for destination and source, both `vec2` and `vec3`.

| Function | Rule |
|---|---|
| `vec3f_from_vec3i(v)`, `vec2f_from_vec2i(v)` | exact |
| `vec3i_from_vec3f(v)`, `vec2i_from_vec2f(v)` | rounded to nearest, saturated |
| `vec3f_from_vec3s(v, scale)`, `vec2f_from_vec2s(v, scale)` | `v * scale`: one int16 step is `scale` units |
| `vec3s_from_vec3f(v, scale)`, `vec2s_from_vec2f(v, scale)` | `v / scale`, rounded, saturated to int16 |
| `vec3f_from_vec3x(v)`, `vec2f_from_vec2x(v)` | `v / 65536` |
| `vec3x_from_vec3f(v)`, `vec2x_from_vec2f(v)` | `v * 65536`, rounded, saturated |
| `vec3i_from_vec3s(v)`, `vec2i_from_vec2s(v)` | exact widening |
| `vec3s_from_vec3i(v)`, `vec2s_from_vec2i(v)` | saturating narrowing |
| `vec3s_from_array(p)` | an `int16_t[3]` position row, exact |

## quat (`f x`)

| Function | Purpose | Formula |
|---|---|---|
| `P_identity()` | the no-rotation quaternion | $`(0,\ 0,\ 0,\ 1)`$ |
| `P_from_axis_angle(axis, angle)` | rotation of `angle` about unit `axis` | $`u = \mathit{axis}\,\sin\tfrac{\mathit{angle}}{2}`$<br> $`w = \cos\tfrac{\mathit{angle}}{2}`$<br> $`(u_x,\ u_y,\ u_z,\ w)`$ |
| `P_mul(a, b)` | composition: `b` first, then `a` | $`a\,b = (a_w \vec b + b_w \vec a + \vec a \times \vec b,\ a_w b_w - \vec a \cdot \vec b)`$<br> with $`\vec a = (a_x, a_y, a_z)`$ |
| `P_from_euler(angles)` | Z, then X, then Y; `angles` is a vec3 of radians (`f`) or turns (`x`) | $`q_y\,q_x\,q_z`$<br> with $`q_k`$ the rotation by $`\mathit{angles}_k`$ about axis $`k`$ |
| `P_rotate(q, v)` | rotates the vec3 `v` by unit `q` | $`v + 2 q_w\,(u \times v) + 2\,u \times (u \times v)`$<br> with $`u = (q_x, q_y, q_z)`$ |
| `P_normalize(q)` | unit length; `q` must not be zero | $`\frac{q}{\sqrt{q_x^2 + q_y^2 + q_z^2 + q_w^2}}`$ |
| `P_from_basis(r, u, f)` | the rotation whose right, up and forward axes are `r`, `u`, `f`; when the trace is not positive, `s` comes from the largest diagonal entry instead<sup>[[53]](../Citations.md#shepperd-1978)</sup> | $`s = 2\sqrt{1 + r_x + u_y + f_z}`$<br> $`\left(\frac{u_z - f_y}{s},\ \frac{f_x - r_z}{s},\ \frac{r_y - u_x}{s},\ \frac{s}{4}\right)`$ |
| `P_nlerp(a, b, t)` | unit-length shortest arc; negate `b` when the 4D dot is negative | $`\mathrm{normalize}(a(1-t)+bt)`$ |
| `quatf_slerp(a, b, t)` | shortest-arc interpolation<sup>[[19]](../Citations.md#shoemake-1985)</sup>, `t` in 0..1; `f` only; `b` is negated first when $`a \cdot b < 0`$ | $`\frac{\sin((1 - t)\,\Omega)}{\sin\Omega}\,a + \frac{\sin(t\,\Omega)}{\sin\Omega}\,b`$<br> with $`\cos\Omega = a \cdot b`$: nearly parallel inputs take `P_nlerp`, the rest evaluated by Eberly's series in cos - 1, halving the arc past a 120-degree rotation ($`a \cdot b < \frac{1}{2}`$) and summing until its terms fall below float precision, with no last-term correction<sup>[[71]](../Citations.md#eberly-2011)</sup> |

`quatf_normalize` and `quatf_nlerp` use `mathf_rsqrt` for the reciprocal length.
A device sweep measures nlerp component error over quaternion pairs and
interpolation weights against the `1 / sqrtf` path.

`quatx_normalize` accepts every nonzero int32 quaternion; zero gives zero.
Its component error is at most $`0.5 + 2^{-12}`$ LSB against exact normalization.

`quatx_unit_normalize` is the near-unit Q16.16 path `quatx_nlerp` uses.

`quatx_unit`'s public second spellings keep the shared template unchanged:
`quatx_unit_normalize`, `quatx_unit_nlerp`, `quatx_unit_from_basis`.
Callers use `quatx_*`, including `quatx_from_basis`.

The unit-input sweep in `suite_math_numbers.c` checks an 8 LSB component
bound against a double reference, including blend and squared-length rounding.

## affine2 (`f x`)

`affine2f_t` and `affine2x_t` hold `x_axis`, `y_axis` and `origin` as vec2 values,
the columns of `L = [a c; b d]` and its translation.

- Points map as `x_axis * p.x + y_axis * p.y + origin`; `mul(a, b)` applies `b` first.
- Fixed-point operations saturate and round each product to Q16.16 before summing.
- Integer-pixel maps sum exact int64 products and round once, without saturation checks.
- The maps read `t` through a pointer, like `mat4_apply`, so inlining copies no struct.
- Rectangle maps return the bounding box of the four corners, exact for an axis-preserving map.

| Function | Purpose | Formula |
|---|---|---|
| `P_identity()` | unit axes, zero origin | $`L = I, o = 0`$ |
| `P_mul(a, b)` | compose, b first | $`L = L_a L_b, o = L_a o_b + o_a`$ |
| `P_determinant(t)` | signed area scale | $`a d - b c`$ |
| `P_invert(t, &out)` | false on zero determinant, out untouched | $`L^{-1} = [d,-c;-b,a]/\det L, o' = -L^{-1}o`$ |
| `P_map_point(&t, p)` | map a point | $`L p + o`$ |
| `P_map_vector(&t, v)` | map a direction, ignore origin | $`L v`$ |
| `P_map_rect(&t, r)` | bounding box in the scalar type | $`\mathrm{bbox}(t(\mathrm{corners}(r)))`$ |
| `affine2f_map_pixel(&t, p)`, `affine2x_map_pixel(&t, p)` | vec2i result, nearest, ties away from zero | $`\mathrm{round}(L p + o)`$ |
| `P_map_recti(&t, r)` | recti bounding box of mapped pixels | $`\mathrm{bbox}(\mathrm{round}(t(\mathrm{corners}(r))))`$ |
| `P_equal(a, b)` | exact comparison of axes and origin | $`L_a = L_b \land o_a = o_b`$ |

## mat3 (`f`)

`mat3f_t` is a row-major linear matrix.

- `mat3f_from_mat4()` takes a mat4's linear part, without translation; `mat3f_mul()` multiplies two.
- `mat3f_cofactor()` is the determinant times the inverse-transpose, with no division.
- It satisfies `cofactor(M) * transpose(M) = det(M) * I`, a reflection's determinant negative.
- Dividing it by the determinant gives the inverse-transpose.
- `mat3f_determinant()` is that det(M): below zero when the matrix mirrors, zero when it is singular.

## mat4 (`f x`)

| Function | Purpose | Formula |
|---|---|---|
| `P_identity()` | the identity matrix | $`I`$ |
| `P_view_from_axes(x, y, z, eye)` | world to the frame whose axes form the rows | $`p' = (x \cdot (p-eye), y \cdot (p-eye), z \cdot (p-eye))`$ |
| `P_scale_rows(m, s)` | scales rows 0 through 2, including translation; bottom row unchanged | $`\mathrm{diag}(s_x, s_y, s_z, 1)\,m`$ |
| `P_scale_columns(m, s)` | scales columns 0 through 2; translation and bottom row unchanged | $`m\,\mathrm{diag}(s_x, s_y, s_z, 1)`$ |
| `P_row_dot_column3(row, &b, c)` | dot of three row entries with column `c` of `b`; used by the matrix products | $`\sum_{k=0}^{2} \mathit{row}_k\,b_{kc}`$ |
| `P_mul(a, b)` | matrix product `a * b` | $`(a\,b)_{rc} = \sum_{k=0}^{3} a_{rk}\,b_{kc}`$ |
| `P_mul_affine(a, b)` | `a * b`, with `b` applied first; both bottom rows must be (0, 0, 0, 1) | $`\begin{pmatrix} A & a \\ 0 & 1 \end{pmatrix}\begin{pmatrix} B & b \\ 0 & 1 \end{pmatrix}`$<br> $`= \begin{pmatrix} A B & A b + a \\ 0 & 1 \end{pmatrix}`$ |
| `P_invert_affine(m)` | float only (opt-in); inverse; bottom row (0, 0, 0, 1), invertible 3x3 part; one divide | $`\begin{pmatrix} M & t \\ 0 & 1 \end{pmatrix}^{-1}`$<br> $`= \begin{pmatrix} M^{-1} & -M^{-1} t \\ 0 & 1 \end{pmatrix}`$<br> $`M^{-1} = \frac{\mathrm{adj} M}{\det M}`$ |
| `P_apply(&m, p)` | transforms the point `p`, translation included | $`p'_r = m_{r0}\,p_x + m_{r1}\,p_y + m_{r2}\,p_z + m_{r3}`$, for $`r = 0, 1, 2`$ |
| `P_from_trs(position, rotation, scale)` | scale, rotate, translate; `rotation` must be unit | $`T\,R\,S = \begin{pmatrix} R\,\mathrm{diag}(\mathit{scale}) & \mathit{position} \\ 0 & 1 \end{pmatrix}`$ |

## transform (`f x`)

| Function | Purpose | Formula |
|---|---|---|
| `P_set_position(&t, p)` | sets position, clears the cache | $`\mathit{position} \leftarrow p`$ |
| `P_set_rotation(&t, q)` | sets rotation (unit), clears the cache | $`\mathit{rotation} \leftarrow q`$ |
| `P_set_scale(&t, s)` | sets scale, clears the cache | $`\mathit{scale} \leftarrow s`$ |
| `P_translate(&t, delta)` | adds `delta` to the position | $`\mathit{position} \leftarrow \mathit{position} + \mathit{delta}`$ |
| `P_rotate(&t, q)` | turns about the transform's own axes | $`\mathit{rotation} \leftarrow \mathrm{normalize}(\mathit{rotation}\;q)`$ |
| `P_matrix(&t)` | the cached model matrix | $`M = T\,R\,S`$ |
| `P_compute_matrix(&t)` | a fresh model matrix from a const transform | $`M = T\,R\,S`$ |
| `P_view(&t)` | the camera's view matrix | $`V = R^{\mathsf T}\,T(-\mathit{position})`$ |
| `transformf_rotate_around(&t, point, axis, angle)` | float only: turns about the line through `point` along `axis`, in the parent's frame; the position swings round `point` and the rotation turns with it | $`q = \mathrm{from\_axis\_angle}(\hat a, \mathit{angle})`$<br> $`\mathit{position} \leftarrow \mathit{point} + q\,(\mathit{position} - \mathit{point})`$<br> $`\mathit{rotation} \leftarrow \mathrm{normalize}(q\;\mathit{rotation})`$ |
| `transformf_compose(&parent, &child)` | float only: the child placed in the parent's frame; exact unless an unevenly scaled parent holds a turned child, where the sheared result has no TRS form and the scale is per axis | $`\mathit{position} = p_t + p_q\,(p_s \odot c_t)`$<br>$`\mathit{rotation} = \mathrm{normalize}(p_q\,c_q)`$<br>$`\mathit{scale} = p_s \odot c_s`$ |
| `transformf_relative(&parent, &world)` | float only: the inverse of `transformf_compose`, the child that placed in `parent` stands at `world`; the parent's scale must be even and nonzero | $`q = \overline{p_q}`$<br>$`\mathit{position} = q\,(w_t - p_t) / p_s`$<br>$`\mathit{rotation} = \mathrm{normalize}(q\,w_q)`$<br>$`\mathit{scale} = w_s / p_s`$ |
| `transformf_scale_is_even(&t)` | float only: the three scales agree within `TRANSFORMF_MATRIX_TOLERANCE` of the largest | $`\max_i s_i - \min_i s_i \le \epsilon \max_i \lvert s_i \rvert`$ |
| `transformf_mirrors(&t)` | float only: an odd count of negative scales, so the transform turns space inside out | $`s_x s_y s_z < 0`$ |
| `transformf_from_mat(m, &t)` | float only: affine TRS decomposition, refuses singular, nonfinite or sheared matrices; a reflection comes back as a negative `scale.x`; orthogonality uses `TRANSFORMF_MATRIX_TOLERANCE` | $`s_i = \lVert M_{:i}\rVert`$<br>$`R_{:i} = M_{:i}/s_i`$<br>$`t = M_{:3}`$<br>Require orthogonal columns; when $`\det R < 0`$, negate $`R_{:0}`$ and $`s_0`$; quaternion from basis<sup>[[53]](../Citations.md#shepperd-1978)</sup> |
| `P_looking(position, forward, up)` | unit-scale pose looking along `forward` from `position` | $`\mathit{look\_along}(\mathit{forward}, \mathit{up})`$ |
| `P_look_along(&t, direction, up)` | faces a nonzero `direction`; `up` must not be parallel to it | $`f = \frac{\mathit{direction}}{\lVert \mathit{direction} \rVert}`$<br> $`r = \frac{\mathit{up} \times f}{\lVert \mathit{up} \times f \rVert}`$<br> $`u = f \times r`$<br> then $`\mathit{rotation} = \mathrm{from\_basis}(r, u, f)`$ |
| `P_look_at(&t, target, up)` | faces `target` with `up` as the sky; `up` must not be parallel to the line to `target`, and `target` must not be the position | $`f = \frac{\mathit{target} - \mathit{position}}{\lVert \mathit{target} - \mathit{position} \rVert}`$<br> $`r = \frac{\mathit{up} \times f}{\lVert \mathit{up} \times f \rVert}`$<br> $`u = f \times r`$<br> then $`\mathit{rotation} = \mathrm{from\_basis}(r, u, f)`$ |

## The transform cache

A `transform` holds position, rotation and scale as the truth and a derived
model matrix with a `cached` flag.

| Call | Effect |
|---|---|
| any setter, `translate`, `rotate`, `look_at`, `rotate_around` | stores the new value and clears `cached` |
| `P_matrix(t)` | rebuilds the matrix only when `cached` is false, then sets it; use for an object read every frame |
| `P_compute_matrix(t)` | a fresh matrix from a const transform; never touches the cache; use in read-only code |
| `P_view(t)` | the inverse of position and rotation (scale ignored), computed every call |

`TRANSFORMF_IDENTITY` / `TRANSFORMX_IDENTITY` are initializers with the cache
valid. A zero-initialized transform has `cached` false, so it builds on first use,
and its zero scale sends every point to the origin. Code never edits `matrix`.

`MAT4F_IDENTITY` / `MAT4X_IDENTITY` initialize a constant matrix, where `P_identity()` cannot.

## Examples

<!-- generated: math-transform-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Transform a point into a camera's view and interpolate a rotation. */
#include "math/linear/transformf.h"

typedef struct {
    vec3f_t in_camera;
    quatf_t halfway;
} transform_example_t;

void
transform_example(transform_example_t* s) {
    transformf_t body = TRANSFORMF_IDENTITY;
    transformf_set_position(&body, (vec3f_t){1.0F, 0.0F, 4.0F});
    transformf_set_rotation(&body, quatf_from_euler((vec3f_t){0.0F, MATH_PI / 2.0F, 0.0F}));

    const mat4f_t model = transformf_matrix(&body); /* built now, kept */
    const vec3f_t world = mat4f_apply(&model, (vec3f_t){0.0F, 0.0F, 1.0F});

    transformf_t camera = TRANSFORMF_IDENTITY;
    transformf_look_at(&camera, world, (vec3f_t){0.0F, 1.0F, 0.0F});
    const mat4f_t view = transformf_view(&camera);
    const vec3f_t in_camera = mat4f_apply(&view, world); /* straight ahead: x = y = 0 */

    const quatf_t halfway = quatf_slerp(quatf_identity(), body.rotation, 0.5F);
    s->in_camera = in_camera;
    s->halfway = halfway;
}
```
<!-- /generated: math-transform-example -->
