# First-order spherical harmonics: sh1.h

`sh1_rgb_t` stores RGB radiance in the real L1 basis order Y00,
Y1−1 (+y), Y10 (+z), Y11 (+x), normalised by `SH1_Y00` and `SH1_Y1`.
The convention follows Sloan.<sup>[[129]](../Citations.md#sloan-2008)</sup>
[Math index](README.md).

`sh1_irradiance_t` holds the cosine-convolved constant and channel-row
gradient matrix. Irradiance is `constant + gradient * normal`.

| Function | Result |
|---|---|
| `sh1_rgb_lerp` | Interpolate corresponding radiance coefficients. |
| `sh1_rgb_add_direction` | Accumulate a direction's radiance times its integration weight. |
| `sh1_rgb_irradiance` | Convolve with the cosine kernel, using `SH1_A0` and `SH1_A1`. |
| `sh1_irradiance_eval` | Evaluate irradiance at a normal. |
| `sh1_rgb_dominant_direction` | Unit L1 gradient direction using caller-supplied channel weights. An isotropic field returns zero. |

[Light Probes](../render/frame/Light-Probes.md) uses them to light
skinned meshes from a baked probe grid.
