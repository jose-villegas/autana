# Geometry

Half-open rectangles and oriented planes. [Math index](README.md).

## Geometry

### rect (`f i s x`)

`P` is `rectf`, `recti`, `rects` or `rectx`. A rectangle stores `x, y, w, h`:
its corner and size, with exclusive right and bottom edges. A nonpositive
width or height is empty. Queries clamp result sizes to zero.

| Function | Purpose | Formula |
|---|---|---|
| `P_right(r)` | exclusive right edge | $`r_x + r_w`$ |
| `P_bottom(r)` | exclusive bottom edge | $`r_y + r_h`$ |
| `P_from_edges(left, top, right, bottom)` | rectangle from half-open edges; crossed edges are empty | $`(left, top, \max(right-left,0), \max(bottom-top,0))`$ |
| `P_is_empty(r)` | a nonpositive extent | $`r_w \le 0 \lor r_h \le 0`$ |
| `P_intersect(a, b)` | shared area, with nonnegative extents | $`l = \max(a_x,b_x),\ t = \max(a_y,b_y)`$<br>$`r = \min(a_x+a_w,b_x+b_w),\ b = \min(a_y+a_h,b_y+b_h)`$<br>$`\mathrm{from\_edges}(l,t,r,b)`$ |
| `P_union(a, b)` | smallest rectangle holding both; empty inputs add nothing; two empty inputs give zero size at a's corner | $`l = \min(a_x,b_x),\ t = \min(a_y,b_y)`$<br>$`r = \max(a_x+a_w,b_x+b_w),\ b = \max(a_y+a_h,b_y+b_h)`$<br>$`\mathrm{from\_edges}(l,t,r,b)`$ for nonempty inputs |
| `P_contains(r, px, py)` | includes left and top, excludes right and bottom | $`r_x \le px < r_x+r_w \land r_y \le py < r_y+r_h`$ |

![Two pixel rectangles with their shaded intersection, dashed union, included left and top edge pixels, excluded right and bottom pixels, and a separate empty crossed-edge example](../images/math/rect-queries.png)

The queries use half-open edges; crossed edges produce an empty rectangle.

### plane (`f x`)

`planef_t` and `planex_t` hold a vec3 `normal` and a scalar `d`; inside is
`dot(normal, point) + d >= 0`. Distances are scaled by the normal's length
until the plane is normalized.

| Function | Purpose | Formula |
|---|---|---|
| `P_from_normal_point(n, p)` | plane through a point with the given normal | $`(n, -n \cdot p)`$ |
| `P_from_points(a, b, c)` | winding determines the inside; unnormalized; the points must not be collinear | $`n = (b-a) \times (c-a),\ d = -n \cdot a`$ |
| `P_distance(p, q)` | signed plane value | $`p_n \cdot q + p_d`$ |
| `P_normalize(p)` | unit normal; normal must be nonzero; for `x`, `dot(normal, normal)` must be representable, as for `vec3x_normalize` | $`(p_n, p_d)/\lVert p_n\rVert`$ |
| `P_from_rows(&m, a, b, c, e)` | the plane $`a x + b y + c z + e \ge 0`$ of affine m's target, in m's source space | $`n_j = a m_{0j} + b m_{1j} + c m_{2j},\ d = a m_{03} + b m_{13} + c m_{23} + e`$ |
| `P_frustum(&m, near_z, center, screen, out)` | near and screen side planes in affine m's source space | `P_from_rows` of each lens-space plane below |

`P_frustum` reads rows as in Gribb and Hartmann<sup>[[77]](../Citations.md#77)</sup>,
with z as the homogeneous w row of a y-down camera with `screen = center + xy / z`.
Each output is `P_from_rows` of one lens-space plane, in this order:

| Index | Plane | $`(a, b, c, e)`$ |
|---|---|---|
| `PLANE_FRUSTUM_NEAR` | `z >= near_z` | $`(0, 0, 1, -\mathit{near\_z})`$ |
| `PLANE_FRUSTUM_LEFT` | screen x at or right of `left` | $`(1, 0, c_x - \text{left}, 0)`$ |
| `PLANE_FRUSTUM_RIGHT` | screen x at or left of `right` | $`(-1, 0, \text{right} - c_x, 0)`$ |
| `PLANE_FRUSTUM_TOP` | screen y at or below `top` | $`(0, 1, c_y - \text{top}, 0)`$ |
| `PLANE_FRUSTUM_BOTTOM` | screen y at or above `bottom` | $`(0, -1, \text{bottom} - c_y, 0)`$ |

`PLANE_FRUSTUM_COUNT` sizes the output. There is no far plane, and the
planes are not normalized.

![Signed plane-value bands before and after normalizing a non-unit normal, sharing the same zero line](../images/math/plane-distance.png)

Normalization preserves the zero line and makes signed values measure perpendicular distance.

![Triangle winding in two panels with opposite normals and green shading on opposite inside sides](../images/math/plane-from-points.png)

Swapping b and c reverses the winding, normal and inside side.

![A left plane and sample point in lens space beside the transformed world-space plane and sample, with equal signed values](../images/math/plane-from-rows.png)

Reading the lens-space plane through the matrix rows preserves its signed value at the mapped point.

![Camera frustum clipped to its axes box beside a ground-plane slice with named zero lines and arrows pointing inside; grid points passing every plane are green](../images/math/frustum.png)

The shown depth bounds the drawing; the frustum has no far plane, and every plane must accept a point.
