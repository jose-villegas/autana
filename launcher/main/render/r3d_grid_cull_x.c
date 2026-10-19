/*
 * r3d_grid_cull_x: six linear bounds on the grid's sums, each stepped from
 * row to row by additions, and the keep spans read from three rows of them.
 */
#include "render/r3d_grid_cull_x.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "math/scalar/mathi.h"

/* Behind the near plane, in front of it, then past the clip's left, right,
 * top and bottom; a side holds only in front. */
#define CULL_BEHIND      0
#define CULL_AHEAD       1
#define CULL_SIDE0       2
#define CULL_BOUNDS      6

/* The sums r3d_x_grid_t admits, and the clip and lens behind them, keep
 * every product of a bound inside an int64_t. */
#define CULL_PIXEL_LIMIT (1 << 20)

/* One bound: a + slope * k <= 0 for the k of a row it holds for. edge is
 * the last such k when slope > 0, the first when slope < 0, and value is
 * a + slope * edge, so a row step moves both by sums alone. */
typedef struct {
    int64_t value;
    int64_t slope;
    int64_t row; /* what a row step adds to a */
    int16_t edge;
} cull_bound_t;

/* One row's k inside each bound: an interval each, empty when lo > hi. */
typedef struct {
    int16_t lo[CULL_BOUNDS], hi[CULL_BOUNDS];
} cull_row_t;

static void
cull_plane_set(int64_t c[4], int64_t x, int64_t y, int64_t z, int64_t w) {
    c[0] = x;
    c[1] = y;
    c[2] = z;
    c[3] = w;
}

/* Bound n as c . U + c[3] <= 0 for a sum U: U.z < (near_z + 1) * one; its
 * opposite; then x * ppu <= left * z, x * ppu >= right * z,
 * -y * ppu <= top * z and -y * ppu >= bottom * z. The sides' pixel offsets
 * from the centre take the projection's own rounding as slack, and a
 * point's z, floor(U.z / one), lies between U.z / one - 1 and U.z / one:
 * each side takes its worse end. */
static void
cull_plane(const r3d_line_view_x_t* view, const recti_t* clip, int n, int64_t c[4]) {
    const int64_t one = 1 << R3D_X_INPUT_SHIFT;
    const int64_t ppu = view->pixels_per_unit;
    const int64_t slack = r3d_x_screen_rounding_px(view);
    const int64_t left = clip->x - view->center_x - slack;
    const int64_t right = recti_right(*clip) - view->center_x + slack;
    const int64_t top = clip->y - view->center_y - slack;
    const int64_t bottom = recti_bottom(*clip) - view->center_y + slack;
    const int64_t z_front = (view->near_z + 1) * one;
    switch (n) {
        case CULL_BEHIND: cull_plane_set(c, 0, 0, 1, 1 - z_front); break;
        case CULL_AHEAD: cull_plane_set(c, 0, 0, -1, z_front); break;
        case CULL_SIDE0: cull_plane_set(c, ppu, 0, -left, (left > 0 ? left : 0) * one); break;
        case CULL_SIDE0 + 1: cull_plane_set(c, -ppu, 0, right, (ppu - (right < 0 ? right : 0)) * one); break;
        case CULL_SIDE0 + 2: cull_plane_set(c, 0, -ppu, -top, (ppu + (top > 0 ? top : 0)) * one); break;
        default: cull_plane_set(c, 0, ppu, bottom, -(bottom < 0 ? bottom : 0) * one); break;
    }
}

/* Bound c at the grid's first row, the lift taking its worse sign. */
static cull_bound_t
cull_bound_make(const int64_t c[4], const r3d_x_grid_t* grid) {
    const int64_t lift = (c[0] * grid->rise.x) + (c[1] * grid->rise.y) + (c[2] * grid->rise.z);
    const int64_t a = (c[0] * grid->base.x) + (c[1] * grid->base.y) + (c[2] * grid->base.z) + c[3]
                      + ((lift < 0 ? -lift : lift) * grid->h_max);
    cull_bound_t b;
    b.slope = (c[0] * grid->step.x) + (c[1] * grid->step.y) + (c[2] * grid->step.z);
    b.row = (c[0] * grid->row.x) + (c[1] * grid->row.y) + (c[2] * grid->row.z);
    b.edge = (int16_t)(b.slope > 0 ? -1 : b.slope < 0 ? grid->k_max + 1 : 0);
    b.value = a + (b.slope * b.edge);
    return b;
}

/* A rising bound's last k that holds, -1 for none: walked from the last
 * row's, as it moves a few k a row. */
static int16_t
cull_bound_last(cull_bound_t* b, int k_max) {
    while (b->edge < k_max && b->value + b->slope <= 0) {
        b->edge++;
        b->value += b->slope;
    }
    while (b->edge >= 0 && b->value > 0) {
        b->edge--;
        b->value -= b->slope;
    }
    return b->edge;
}

/* A falling bound's first k that holds, k_max + 1 for none. */
static int16_t
cull_bound_first(cull_bound_t* b, int k_max) {
    while (b->edge > 0 && b->value - b->slope <= 0) {
        b->edge--;
        b->value -= b->slope;
    }
    while (b->edge <= k_max && b->value > 0) {
        b->edge++;
        b->value += b->slope;
    }
    return b->edge;
}

/* The k of this row a bound holds for. */
static void
cull_bound_settle(cull_bound_t* b, int k_max, int16_t* lo, int16_t* hi) {
    *lo = 0;
    *hi = (int16_t)k_max;
    if (b->slope > 0) {
        *hi = cull_bound_last(b, k_max);
    } else if (b->slope < 0) {
        *lo = cull_bound_first(b, k_max);
    } else if (b->value > 0) {
        *hi = -1;
    }
}

/* The current row's intervals; then steps every bound to the next row. */
static void
cull_row(cull_bound_t bounds[CULL_BOUNDS], int k_max, cull_row_t* row) {
    for (int n = 0; n < CULL_BOUNDS; n++) {
        cull_bound_settle(&bounds[n], k_max, &row->lo[n], &row->hi[n]);
        bounds[n].value += bounds[n].row;
    }
    for (int n = CULL_SIDE0; n < CULL_BOUNDS; n++) {
        row->lo[n] = (int16_t)mathi_max(row->lo[n], row->lo[CULL_AHEAD]);
        row->hi[n] = (int16_t)mathi_min(row->hi[n], row->hi[CULL_AHEAD]);
    }
}

/* The k of `row` whose row neighbours, where they exist, and the same k
 * of `above` and `below` (NULL past the grid's ends) all lie in bound n's
 * interval too. Being in front alone hides nothing. */
static void
cull_drop(const cull_row_t* above, const cull_row_t* row, const cull_row_t* below, int n, int k_max, int16_t* lo,
          int16_t* hi) {
    *lo = (int16_t)(row->lo[n] == 0 ? 0 : row->lo[n] + 1);
    *hi = (int16_t)(n == CULL_AHEAD ? -1 : row->hi[n] == k_max ? k_max : row->hi[n] - 1);
    if (above != NULL) {
        *lo = (int16_t)mathi_max(*lo, above->lo[n]);
        *hi = (int16_t)mathi_min(*hi, above->hi[n]);
    }
    if (below != NULL) {
        *lo = (int16_t)mathi_max(*lo, below->lo[n]);
        *hi = (int16_t)mathi_min(*hi, below->hi[n]);
    }
}

/* The k of `row` left once every k dropped from either end is trimmed. */
static void
cull_keep(const cull_row_t* above, const cull_row_t* row, const cull_row_t* below, int k_max, int8_t keep[2]) {
    int16_t drop_lo[CULL_BOUNDS];
    int16_t drop_hi[CULL_BOUNDS];
    for (int n = 0; n < CULL_BOUNDS; n++) {
        cull_drop(above, row, below, n, k_max, &drop_lo[n], &drop_hi[n]);
    }
    int lo = 0;
    int hi = k_max;
    for (bool moved = true; moved && lo <= hi;) {
        moved = false;
        for (int n = 0; n < CULL_BOUNDS; n++) {
            if (drop_lo[n] <= lo && drop_hi[n] >= lo) {
                lo = drop_hi[n] + 1;
                moved = true;
            }
            if (lo <= hi && drop_lo[n] <= hi && drop_hi[n] >= hi) {
                hi = drop_lo[n] - 1;
                moved = true;
            }
        }
    }
    keep[0] = (int8_t)lo;
    keep[1] = (int8_t)hi;
}

void
r3d_x_grid_cull_spans(const r3d_line_view_x_t* view, const recti_t* clip, const r3d_x_grid_t* grid, int8_t keep[][2]) {
    assert(grid->k_max >= 0 && grid->k_max <= R3D_X_GRID_K_MAX && grid->r_max >= 0 && grid->r_max <= R3D_X_GRID_K_MAX
           && grid->h_max >= 0);
    assert(view->pixels_per_unit > 0 && view->pixels_per_unit < CULL_PIXEL_LIMIT
           && mathi_abs(clip->x) < CULL_PIXEL_LIMIT && mathi_abs(clip->y) < CULL_PIXEL_LIMIT
           && clip->w < CULL_PIXEL_LIMIT && clip->h < CULL_PIXEL_LIMIT);
    cull_bound_t bounds[CULL_BOUNDS];
    for (int n = 0; n < CULL_BOUNDS; n++) {
        int64_t c[4];
        cull_plane(view, clip, n, c);
        bounds[n] = cull_bound_make(c, grid);
    }
    cull_row_t rows[3];
    cull_row(bounds, grid->k_max, &rows[1]);
    for (int r = 0; r <= grid->r_max; r++) {
        const cull_row_t* above = r > 0 ? &rows[r % 3] : NULL;
        const cull_row_t* below = NULL;
        if (r < grid->r_max) {
            cull_row(bounds, grid->k_max, &rows[(r + 2) % 3]);
            below = &rows[(r + 2) % 3];
        }
        cull_keep(above, &rows[(r + 1) % 3], below, grid->k_max, keep[r]);
    }
}
