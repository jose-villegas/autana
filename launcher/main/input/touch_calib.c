#include "input/touch_calib.h"

#include <math.h>

#define ONE 65536.0f

static int32_t
fixed(float v) {
    return (int32_t)lroundf(v * ONE);
}

touch_calib_t
touch_calib_from_fit(touch_calib_fit_t fit) {
    const float det = fit.xx * fit.yy - fit.xy * fit.yx;
    const float ixx = fit.yy / det;
    const float ixy = -fit.xy / det;
    const float iyx = -fit.yx / det;
    const float iyy = fit.xx / det;
    return (touch_calib_t){
        .xx = fixed(ixx),
        .xy = fixed(ixy),
        .x0 = fixed(-(ixx * fit.x0 + ixy * fit.y0)),
        .yx = fixed(iyx),
        .yy = fixed(iyy),
        .y0 = fixed(-(iyx * fit.x0 + iyy * fit.y0)),
    };
}

static int
clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int
round_fixed(int64_t v) {
    return (int)((v + 32768) >> 16);
}

void
touch_calib_apply(const touch_calib_t* calib, int w, int h, int* x, int* y) {
    const int64_t rx = *x;
    const int64_t ry = *y;
    *x = clamp(round_fixed(calib->xx * rx + calib->xy * ry + calib->x0), 0, w - 1);
    *y = clamp(round_fixed(calib->yx * rx + calib->yy * ry + calib->y0), 0, h - 1);
}
