/* circlei: a closed disc of an int. */
#pragma once
#include "math/geometry/circle_template.h"
#include "math/geometry/recti.h"
#include "math/linear/vec2i.h"

MATH_DEFINE_CIRCLE(circlei, vec2i, recti, int, mathi, mathi_one())

/* cell_count = sum(dy=-r..r, 2*floor(sqrt(r^2-dy^2))+1); negative r gives zero, result must fit int. */
static inline int
circlei_cell_count(int radius) {
    if (radius < 0) {
        return 0;
    }
    int x = radius, count = 0;
    for (int y = 0; y <= radius; y++) {
        while (!mathi_in_disc(x, y, radius)) {
            x--;
        }
        const int row = x + x + 1;
        count += y == 0 ? row : row + row;
    }
    return count;
}
