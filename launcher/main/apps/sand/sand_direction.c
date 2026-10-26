/* Retains the last meaningful gravity direction while the board is nearly flat. */

#include "sand.h"

#include "input/motion/gravity.h"
#include "sand_controls.h"

bool
sand_gravity_direction_changed(int gx, int gy, int strength, int* dx, int* dy) {
    if (strength < (int)(GRAVITY_MIN_DIRECTION_STRENGTH * SAND_FLOW_ONE)) {
        return false;
    }
    int next_dx;
    int next_dy;
    sand_gravity_direction(gx, gy, &next_dx, &next_dy);
    if (next_dx == *dx && next_dy == *dy) {
        return false;
    }
    *dx = next_dx;
    *dy = next_dy;
    return true;
}
