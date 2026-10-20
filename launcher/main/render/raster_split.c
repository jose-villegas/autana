#include "render/raster_split.h"

#include <assert.h>

#include "math/scalar/mathi.h"

#define SPLIT_DAMPING_DIVISOR 2.0F

void
raster_split_forget(raster_split_t* split) {
    *split = (raster_split_t){0};
}

int
raster_split_update(int top_permille, int64_t top_us, int64_t bottom_us) {
    const int share = top_permille;
    assert(share >= RASTER_SPLIT_MIN && share <= RASTER_SPLIT_MAX);
    if (top_us <= 0 || bottom_us <= 0) {
        return share;
    }
    const float rate_top = (float)top_us / (float)share;
    const float rate_bottom = (float)bottom_us / (float)(RASTER_SPLIT_PERMILLE - share);
    const float balanced = (float)RASTER_SPLIT_PERMILLE * rate_bottom / (rate_top + rate_bottom);
    return mathi_clamp((int)(((float)share + balanced) / SPLIT_DAMPING_DIVISOR), RASTER_SPLIT_MIN, RASTER_SPLIT_MAX);
}
