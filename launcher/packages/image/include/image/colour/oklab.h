/* oklab: the OKLab perceptual colour space (Ottosson 2020) from and to linear-light sRGB. */
#pragma once

#include <math.h>

#include "math/linear/vec3f.h"

typedef struct {
    float l;
    float a;
    float b;
} oklabf_t;

static inline oklabf_t
oklabf_from_linear(vec3f_t c) {
    const float l = cbrtf((0.4122214708F * c.x) + (0.5363325363F * c.y) + (0.0514459929F * c.z));
    const float m = cbrtf((0.2119034982F * c.x) + (0.6806995451F * c.y) + (0.1073969566F * c.z));
    const float s = cbrtf((0.0883024619F * c.x) + (0.2817188376F * c.y) + (0.6299787005F * c.z));
    return (oklabf_t){(0.2104542553F * l) + (0.7936177850F * m) - (0.0040720468F * s),
                      (1.9779984951F * l) - (2.4285922050F * m) + (0.4505937099F * s),
                      (0.0259040371F * l) + (0.7827717662F * m) - (0.8086757660F * s)};
}

/* Out-of-gamut colours come back outside [0, 1]; the caller clamps or rejects. */
static inline vec3f_t
oklabf_to_linear(oklabf_t lab) {
    const float l = lab.l + (0.3963377774F * lab.a) + (0.2158037573F * lab.b);
    const float m = lab.l - (0.1055613458F * lab.a) - (0.0638541728F * lab.b);
    const float s = lab.l - (0.0894841775F * lab.a) - (1.2914855480F * lab.b);
    const float l3 = l * l * l;
    const float m3 = m * m * m;
    const float s3 = s * s * s;
    return (vec3f_t){(4.0767416621F * l3) - (3.3077115913F * m3) + (0.2309699292F * s3),
                     (-1.2684380046F * l3) + (2.6097574011F * m3) - (0.3413193965F * s3),
                     (-0.0041960863F * l3) - (0.7034186147F * m3) + (1.7076147010F * s3)};
}
