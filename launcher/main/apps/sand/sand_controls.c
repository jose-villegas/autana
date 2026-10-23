#include "apps/sand/sand_controls.h"

#include <math.h>

#include "apps/sand/sand.h"
#include "math/scalar/mathf.h"

int
sand_controls_jostle(float level) {
    return (int)lroundf(mathf_clamp(level, 0.0F, 1.0F) * SAND_JOSTLE_MAX);
}
