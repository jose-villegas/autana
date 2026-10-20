# Bake Method

How a bake's light is computed and how a bake is scored against its source:
local occlusion, bounced light, and the fidelity numbers. Measured results are
[Bake-Quality.md](Bake-Quality.md); the settings are in [Scene-Files.md](Scene-Files.md).

## Local-occlusion implementation

The [`ao` setting](Scene-Files.md#bake-ao) scales a baked point's ambient light,
and with `indirect = true` its bounced light, by a factor from short
rays, a distance-limited obscurance<sup>[[14]](../Citations.md#14)</sup>. Each point $x$ casts
$R$ cosine-weighted rays, the same directions in its own frame as the bounce
rays; the ray $i$ that hits a surface at distance
$`t_i`$ within the reach $D$ has weight $`w_i = 1 - t_i/D`$, any other ray
$`w_i = 0`$. With strength $s$ the factor is

```math
f(x) = 1 - \frac{s}{R} \sum_{i=1}^{R} w_i
```

A double-sided surface has no side it is meant to be seen from, so it takes the
larger $f$ of its two sides. Sun and sky visibility use their own rays and are
not scaled.

## Bounced-light implementation

The [bake recipe](Scene-Files.md#bake-indirect) enables diffuse bounce light
in the same vertex or face colours as direct light. The renderer reads one
colour, so frame cost and mesh size do not change; only the bake takes longer.

The bake exports the full-detail source mesh, with its textures at full
resolution, and the scene's directional and sky lights to Mitsuba once. A baked
point $x$, a smooth vertex or a flat face sample, sends $R$ cosine-weighted
rays into that scene. Mitsuba's<sup>[[15]](../Citations.md#15)</sup> path integrator<sup>[[16]](../Citations.md#16)</sup> follows each ray for up to $K$
bounces with next-event estimation at every hit, so the hit's own shadow, albedo
and further bounces are all in what comes back. With $`L_i(x)`$ the light
gathered along ray $i$, the mean is the bounced irradiance over $\pi$, and the
albedo, the tone map and the encode follow the direct light:

```math
E_{\mathrm{ind}}(x) = \frac{1}{R} \sum_{i=1}^{R} L_i(x),
\qquad
L(x) = a(x)\,\bigl(E_{\mathrm{direct}}(x) + E_{\mathrm{ind}}(x)\bigr)
```

Light that reaches $x$ without a bounce is $`E_{\mathrm{direct}}`$: one shadow ray
toward the sun, which is a point source and so casts hard shadows, the share of
the sky light's fixed rays that reach the sky, and the ambient light. A ray that hits nothing adds
nothing to $`E_{\mathrm{ind}}`$, because the sky light already counts the sky.
With every albedo at most $\rho \lt 1$ each bounce adds less than the one before;
pick $K$ where the next bounce is negligible beside the direct light.

Every point uses the same $R$ first directions, laid out in its own tangent
frame, and the path integrator's own random numbers come from a fixed seed, so
the same tree and the same partition of points give the same bytes. The noise the paths leave in the bounced light
falls as $1/\sqrt{R}$. A smooth bake gathers once for the vertex copies a crease
splits at one position, on their mean normal, and gives every copy that
bounced term: bounced light changes slowly where direct light does not, and
copies that differ only in it would stop merging into one vertex. A
double-sided surface is lit on both sides, bounced light included, and keeps the
brighter, so a card in the sun takes its sunlit side and a curtain in shade
takes its open side; a ray that reaches a one-sided triangle from behind finds
no light, so light does not pass through shells.

The [indirect look](Scene-Files.md#indirect) controls bounced intensity and
bounce reflectance.

The cost is $R$ paths of up to $K+1$ segments per baked point. The limit is the
light's resolution: it is the vertex or face spacing of the baked mesh, so
bounce detail smaller than a triangle is lost, and a coloured surface tints
only the triangles it reaches. A scene's bounce sweep, scores and images live
beside its own tools.

## Fidelity against a reference

How close is a bake to the ground truth, and where is it off? The reference
renderer lights the full source mesh, per pixel, with the scene's own lights,
shadows and tone map, at the device render size and supersampled, for the
poses of the scene's camera path. A host render of any variant, smooth, lite
or flat, is scored against it:

| Number | Meaning |
|---|---|
| Mean ΔE76 | CIE76 colour difference per pixel, averaged; about 2 is just visible, 20 is a clearly different colour |
| p95 ΔE76 | The 95th percentile of the pixels' ΔE, averaged over frames: how bad the worst places are |
| Luma SSIM | Structural similarity over 8x8 windows of luma, 1 for identical: whether shapes and contrast match |
| Edge and interior ΔE76 | The same ΔE on pixels at a sharp luma step of the reference (a silhouette, a lit or shadowed boundary) and on all others |

Heatmaps put each pixel's ΔE on a scale from black (a match) through red
(about 20) to yellow (50 or more). The sheet beside them shows the reference,
the render, the heatmap and the edge pixels, and the commands that make all of
it are in [`launcher/tools/r3d/README.md`](../../launcher/tools/r3d/README.md#fidelity-reference).
A scene's scores live beside its own tools, and its example sheets are
regenerated with the other doc images.

The ceiling for a flat bake is the smooth bake's own error. Smooth, one colour
per vertex, is not exact either, and flat adds the colour gradient across each
triangle, which no face sampling brings back. What face sampling does change is
how well the one colour represents the face.

### The scores, exactly

A pixel's 8-bit colour $c$ is decoded with the display gamma $\gamma = 2.2$
rather than the piecewise sRGB curve, taken to CIE XYZ through the linear sRGB
primaries<sup>[[10]](../Citations.md#10)</sup>, and to CIELAB relative to the D65 white<sup>[[4]](../Citations.md#4)</sup>.
The constants live in `render_compare.py`, which both the
scoring and the fit's loss read.

```math
\ell = \left(\frac{c}{255}\right)^{\gamma},
\qquad
\begin{pmatrix}X\\Y\\Z\end{pmatrix} =
\begin{pmatrix}
0.4124564 & 0.3575761 & 0.1804375\\
0.2126729 & 0.7151522 & 0.0721750\\
0.0193339 & 0.1191920 & 0.9503041
\end{pmatrix}\ell
```

```math
f(t) = \begin{cases} t^{1/3} & t > \delta^3\\[2pt] \dfrac{t}{3\delta^2} + \dfrac{4}{29} & \text{otherwise}\end{cases},
\qquad \delta = \frac{6}{29},
\qquad (X_n, Y_n, Z_n) = (0.95047,\ 1,\ 1.08883)
```

```math
L^* = 116\,f\!\left(\tfrac{Y}{Y_n}\right) - 16,
\qquad a^* = 500\left(f\!\left(\tfrac{X}{X_n}\right) - f\!\left(\tfrac{Y}{Y_n}\right)\right),
\qquad b^* = 200\left(f\!\left(\tfrac{Y}{Y_n}\right) - f\!\left(\tfrac{Z}{Z_n}\right)\right)
```

```math
\Delta E_{76}(p) = \left\lVert \mathrm{Lab}(R_p) - \mathrm{Lab}(T_p) \right\rVert_2
```

for render $R$ and reference $T$ at pixel $p$. Mean ΔE averages
$`\Delta E_{76}(p)`$ over the frame's pixels and p95 is its 95th percentile.
Luma SSIM<sup>[[9]](../Citations.md#9)</sup> works on the gamma-encoded luma
$y = 0.2126 r + 0.7152 g + 0.0722 b$ (channels 0 to 1), over every unweighted
8 by 8 window $w$ of the frame where the paper uses 11 by 11 Gaussian-weighted
ones, with the window's means $\mu$, variances $\sigma^2$ and covariance
$`\sigma_{RT}`$; the constants are the paper's:

```math
\mathrm{SSIM} = \frac{1}{|W|}\sum_{w \in W}
\frac{(2\mu_R\mu_T + C_1)(2\sigma_{RT} + C_2)}{(\mu_R^2 + \mu_T^2 + C_1)(\sigma_R^2 + \sigma_T^2 + C_2)},
\qquad C_1 = 0.01^2,\quad C_2 = 0.03^2
```

The edge pixels are those within one pixel of a step in the reference's luma
steeper than 0.06 a pixel; edge ΔE averages $`\Delta E_{76}`$ over them and
interior ΔE over the rest:

```math
E = \mathrm{dilate}_1\left\{\, p : \left\lVert \nabla y_T(p) \right\rVert > 0.06 \,\right\}
```

Appearance fitting, its objective, and its budget and cost choices are
described by [fit.prune](Scene-Files.md#fitprune) and
[fit.optimise](Scene-Files.md#fitoptimise).
