/* Lighten a 24-bit sRGB colour in OKLab, where a lightness step looks even. */
#include "image/colour/oklab.h"
#include "image/colour/srgb.h"

uint32_t
colour_example_lighten(uint32_t rgb, float step) {
    const vec3f_t linear = {srgbf_decode_byte((uint8_t)(rgb >> 16)), srgbf_decode_byte((uint8_t)(rgb >> 8)),
                            srgbf_decode_byte((uint8_t)rgb)};
    oklabf_t lab = oklabf_from_linear(linear);
    lab.l += step;
    const vec3f_t out = oklabf_to_linear(lab); /* may leave [0, 1]; encode clamps */
    return ((uint32_t)srgbf_encode_byte(out.x) << 16) | ((uint32_t)srgbf_encode_byte(out.y) << 8)
           | srgbf_encode_byte(out.z);
}
