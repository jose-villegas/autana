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

void
touch_calib_apply(const touch_calib_t* calib, int w, int h, vec2i_t* point) {
    const int64_t rx = point->x;
    const int64_t ry = point->y;
    point->x = mathi_clamp(round_fixed((calib->xx * rx) + (calib->xy * ry) + calib->x0), 0, w - 1);
    point->y = mathi_clamp(round_fixed((calib->yx * rx) + (calib->yy * ry) + calib->y0), 0, h - 1);
}

void
touch_calib_prepare_point(bool injected, bool have_point, vec2i_t* point, bool calibrate, const touch_calib_t* calib,
                          int w, int h) {
    if (have_point && !injected && calibrate) {
        touch_calib_apply(calib, w, h, point);
    }
}
