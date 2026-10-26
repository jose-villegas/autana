#include "input/touch/touch_calib.h"

#include <math.h>

#include "math/scalar/mathi.h"
#include "math/scalar/mathx.h"

#define ONE ((float)MATHX_ONE)

static int32_t
fixed(float v) {
    return (int32_t)lroundf(v * ONE);
}

touch_calib_t
touch_calib_from_fit(touch_calib_fit_t fit) {
    const float det = (fit.xx * fit.yy) - (fit.xy * fit.yx);
    const float ixx = fit.yy / det;
    const float ixy = -fit.xy / det;
    const float iyx = -fit.yx / det;
    const float iyy = fit.xx / det;
    return (touch_calib_t){
        .xx = fixed(ixx),
        .xy = fixed(ixy),
        .x0 = fixed(-((ixx * fit.x0) + (ixy * fit.y0))),
        .yx = fixed(iyx),
        .yy = fixed(iyy),
        .y0 = fixed(-((iyx * fit.x0) + (iyy * fit.y0))),
    };
}

static int
round_fixed(int64_t v) {
    return (int)((v + MATHX_ONE / 2) >> MATHX_SHIFT);
}

vec2i_t
touch_calib_apply(const touch_calib_t* calib, int w, int h, vec2i_t raw) {
    return (vec2i_t){
        mathi_clamp(round_fixed(((int64_t)calib->xx * raw.x) + ((int64_t)calib->xy * raw.y) + calib->x0), 0, w - 1),
        mathi_clamp(round_fixed(((int64_t)calib->yx * raw.x) + ((int64_t)calib->yy * raw.y) + calib->y0), 0, h - 1),
    };
}

void
touch_calib_prepare_point(bool injected, bool have_point, vec2i_t* point, bool calibrate, const touch_calib_t* calib,
                          int w, int h) {
    if (have_point && !injected && calibrate) {
        *point = touch_calib_apply(calib, w, h, *point);
    }
}
