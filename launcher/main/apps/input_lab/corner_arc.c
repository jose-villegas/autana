#include "apps/input_lab/corner_arc.h"

#include <math.h>

int
corner_arc_inset(int radius, int row) {
    if (row >= radius) {
        return 0;
    }
    const float up = (float)(radius - row);
    return (int)lroundf((float)radius - sqrtf((float)radius * (float)radius - up * up));
}
