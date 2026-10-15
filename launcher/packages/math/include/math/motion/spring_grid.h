/* A caller-owned Q16 spring sheet with free edges and velocity in units per tick.
 * For zero damping, symplectic Euler is stable when
 * (8 * tension + stiffness) / SPRING_COEFFICIENT_ONE < 4.
 * Damping scales the updated velocity and must be in 0..SPRING_COEFFICIENT_ONE.
 */
#pragma once

#include <stdint.h>

#include "math/motion/spring.h"
#include "math/scalar/mathx.h"

typedef struct {
    int32_t* offset;
    int32_t* velocity;
    int width, height;
    int tension, stiffness, damping;
} spring_grid_t;

static inline void
spring_grid_init(spring_grid_t* grid, int32_t* offset, int32_t* velocity, int width, int height, int tension,
                 int stiffness, int damping) {
    *grid = (spring_grid_t){.offset = offset,
                            .velocity = velocity,
                            .width = width,
                            .height = height,
                            .tension = tension,
                            .stiffness = stiffness,
                            .damping = damping};
    for (int i = 0; i < width * height; i++) {
        offset[i] = 0;
        velocity[i] = 0;
    }
}

static inline void
spring_grid_poke(spring_grid_t* grid, int x, int y, int radius, int32_t velocity) {
    if (radius < 1) {
        radius = 1;
    }
    for (int j = 0; j < grid->height; j++) {
        for (int i = 0; i < grid->width; i++) {
            const int64_t dx = (int64_t)i - x;
            const int64_t dy = (int64_t)j - y;
            const int window = spring_bump_window((dx * dx) + (dy * dy), radius);
            grid->velocity[(j * grid->width) + i] += spring_scale(velocity, window);
        }
    }
}

static inline int32_t
spring_grid_laplacian(const spring_grid_t* grid, int x, int y) {
    const int i = (y * grid->width) + x;
    const int32_t here = grid->offset[i];
    const int32_t left = grid->offset[x > 0 ? i - 1 : i];
    const int32_t right = grid->offset[x + 1 < grid->width ? i + 1 : i];
    const int32_t up = grid->offset[y > 0 ? i - grid->width : i];
    const int32_t down = grid->offset[y + 1 < grid->height ? i + grid->width : i];
    return left + right + up + down - (4 * here);
}

static inline void
spring_grid_tick(spring_grid_t* grid) {
    for (int y = 0; y < grid->height; y++) {
        for (int x = 0; x < grid->width; x++) {
            const int i = (y * grid->width) + x;
            int32_t v = grid->velocity[i];
            v += spring_scale(spring_grid_laplacian(grid, x, y), grid->tension);
            v -= spring_scale(grid->offset[i], grid->stiffness);
            v -= spring_scale(v, grid->damping);
            grid->velocity[i] = v;
        }
    }
    for (int i = 0; i < grid->width * grid->height; i++) {
        grid->offset[i] += grid->velocity[i];
    }
}
