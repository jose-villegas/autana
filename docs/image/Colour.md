# Colour: srgb.h and oklab.h

The sRGB transfer function and the OKLab colour space, header-only, in float.
[Image package](../../launcher/packages/image/README.md).

[`srgb.h`](../../launcher/packages/image/include/image/colour/srgb.h) is the piecewise curve of IEC 61966-2-1,<sup>[[10]](../Citations.md#iec-61966-2-1)</sup>
between linear light $`c`$ and an encoded value $`e`$, both in $`[0, 1]`$:

```math
c = \begin{cases} e / 12.92 & e \le 0.04045 \\ \left(\frac{e + 0.055}{1.055}\right)^{2.4} & \text{otherwise} \end{cases}
\qquad
e = \begin{cases} 12.92\,c & c \le 0.0031308 \\ 1.055\,c^{1/2.4} - 0.055 & \text{otherwise} \end{cases}
```

| Function | Result |
|---|---|
| `srgbf_decode` | Encoded value to linear light |
| `srgbf_encode` | Linear light to an encoded value, clamped to $`[0, 1]`$ first; NaN gives 0 |
| `srgbf_decode_byte` | An 8-bit encoded channel to linear light |
| `srgbf_encode_byte` | Linear light to the nearest 8-bit encoded channel; every byte round-trips |

[`oklab.h`](../../launcher/packages/image/include/image/colour/oklab.h) converts between linear-light sRGB and OKLab with Ottosson's
matrices.<sup>[[35]](../Citations.md#ottosson-2020)</sup> `oklabf_t` holds
lightness `l` and the opponent axes `a` and `b`, unscaled: white is
$`(1, 0, 0)`$.

| Function | Result |
|---|---|
| `oklabf_from_linear` | Linear sRGB to OKLab |
| `oklabf_to_linear` | OKLab to linear sRGB; out-of-gamut colours fall outside $`[0, 1]`$ for the caller to clamp or reject |
| `oklabf_from_srgb_bytes` | An 8-bit encoded sRGB colour to OKLab |
| `oklabf_distance_sq` | The squared distance between two OKLab colours, OKLab's colour difference |

## Example

<!-- generated: image-colour-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Lighten a 24-bit sRGB colour in OKLab, where a lightness step looks even. */
#include "image/colour/oklab.h"
#include "image/colour/srgb.h"

uint32_t
colour_example_lighten(uint32_t rgb, float step) {
    oklabf_t lab = oklabf_from_srgb_bytes((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
    lab.l += step;
    const vec3f_t out = oklabf_to_linear(lab); /* may leave [0, 1]; encode clamps */
    return ((uint32_t)srgbf_encode_byte(out.x) << 16) | ((uint32_t)srgbf_encode_byte(out.y) << 8)
           | srgbf_encode_byte(out.z);
}
```
<!-- /generated: image-colour-example -->
