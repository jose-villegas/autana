/*
 * Portable suite: the gravity-ordered chunk schedule - the cut, the order a
 * travel direction implies, and what two lanes stepping it may overlap.
 *
 * Every claim is swept over the grid sizes, chunk offsets, travel
 * directions and phases a pass can ask for, because the interesting
 * failures sit at the edges: a short first chunk, a short last one, and the
 * four diagonal directions.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_chunk_sched.h"

#define CS_OFFSETS     5
#define CS_LARGE_GRIDS 3
#define CS_INTERLEAVES 6
#define CS_LINE_COST   10

/* Grid, then the side on each axis. The last two are non-square, both ways
 * round: a cut wider than it is tall and one taller than it is wide. */
static const int cs_grids[][4] = {
    {184, 224, 35, 35}, {122, 149, 23, 23}, {92, 112, 17, 17}, {61, 74, 17, 17},
    {46, 56, 17, 17},   {92, 112, 31, 17},  {92, 112, 17, 37},
};

static const int cs_dirs[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

static const uint32_t cs_seeds[] = {1u, 7u, 12345u};

#define CS_GRIDS ((int)(sizeof cs_grids / sizeof cs_grids[0]))
#define CS_DIRS  ((int)(sizeof cs_dirs / sizeof cs_dirs[0]))

typedef struct {
    sand_chunk_plan_t plan;
    sand_chunk_order_t order;
    int tx, ty;
    unsigned phase;
    char label[64];
} cs_case_t;

typedef void (*cs_check_fn)(const cs_case_t* c, void* ctx);

typedef struct {
    const cs_case_t* c;
    sand_chunk_sched_t k;
    uint8_t running[SAND_CHUNKS_MAX];
    uint8_t ran[SAND_CHUNKS_MAX];
    uint8_t seq[SAND_CHUNKS_MAX];
    int seq_n;
    int depth;
    bool reenter;
} cs_run_t;

static cs_run_t run;

static void
fixture(void) {
    for (int i = 0; i < SAND_CHUNKS_MAX; i++) {
        run.running[i] = 0;
        run.ran[i] = 0;
        run.seq[i] = 0;
    }
    run.c = NULL;
    run.seq_n = 0;
    run.depth = 0;
    run.reenter = false;
}

static void
cs_offset(int which, int side_x, int side_y, int* off_x, int* off_y) {
    switch (which) {
        case 1:
            *off_x = 1;
            *off_y = 0;
            break;
        case 2:
            *off_x = 0;
            *off_y = side_y - 1;
            break;
        case 3:
            *off_x = side_x / 2;
            *off_y = side_y / 3;
            break;
        case 4:
            *off_x = side_x - 1;
            *off_y = side_y - 1;
            break;
        default:
            *off_x = 0;
            *off_y = 0;
            break;
    }
}

static void
cs_build(cs_case_t* c, int grid, int which_off) {
    const int side_x = cs_grids[grid][2];
    const int side_y = cs_grids[grid][3];
    int off_x, off_y;

    cs_offset(which_off, side_x, side_y, &off_x, &off_y);
    TEST_ASSERT_TRUE_MESSAGE(
        sand_chunk_plan(&c->plan, cs_grids[grid][0], cs_grids[grid][1], side_x, side_y, off_x, off_y),
        "every grid and offset this suite sweeps must plan");
}

static void
cs_set_pass(cs_case_t* c, int dir, unsigned phase) {
    c->tx = cs_dirs[dir][0];
    c->ty = cs_dirs[dir][1];
    c->phase = phase;
    sand_chunk_order(&c->order, c->plan.cols, c->plan.rows, c->tx, c->ty, phase);
    snprintf(c->label, sizeof c->label, "%dx%d/%dx%d off %d,%d dir %d,%d phase %u", c->plan.w, c->plan.h,
             c->plan.side_x, c->plan.side_y, c->plan.off_x, c->plan.off_y, c->tx, c->ty, phase);
}

static void
cs_sweep(int grids, cs_check_fn check, void* ctx) {
    for (int g = 0; g < grids; g++) {
        for (int oi = 0; oi < CS_OFFSETS; oi++) {
            cs_case_t c;
            cs_build(&c, g, oi);
            for (int d = 0; d < CS_DIRS; d++) {
                for (unsigned phase = 0; phase < 2u; phase++) {
                    cs_set_pass(&c, d, phase);
                    check(&c, ctx);
                }
            }
        }
    }
}

/* Chunk indices of the existing 8-neighbours of (cx, cy). */
static int
cs_neighbours(int cols, int rows, int cx, int cy, int* out) {
    static const int ring[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    int n = 0;

    for (int i = 0; i < 8; i++) {
        const int nx = cx + ring[i][0];
        const int ny = cy + ring[i][1];
        if ((unsigned)nx < (unsigned)cols && (unsigned)ny < (unsigned)rows) {
            out[n++] = ny * cols + nx;
        }
    }
    return n;
}

/* Index counted from an axis's downstream end - the suite's own copy of
 * what a diagonal wave is numbered by, kept apart from the builder's. */
static int
cs_downstream(int c, int n, int t) {
    return (t > 0) ? (n - 1 - c) : c;
}

/* the cut */

static void
cs_tally_chunk(const cs_case_t* c, uint8_t* seen, int cx, int cy, const char* label) {
    int x0, x1, y0, y1;
    sand_chunk_cells(&c->plan, cx, cy, &x0, &x1, &y0, &y1);

    if (cx > 0 && cx < c->plan.cols - 1) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->plan.side_x, x1 - x0, label);
    }
    if (cy > 0 && cy < c->plan.rows - 1) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->plan.side_y, y1 - y0, label);
    }
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            seen[y * c->plan.w + x]++;
        }
    }
}

static void
cs_check_partition(const cs_case_t* c, const char* label) {
    const int cells = c->plan.w * c->plan.h;
    uint8_t* seen = calloc((size_t)cells, 1);
    TEST_ASSERT_NOT_NULL(seen);

    for (int cy = 0; cy < c->plan.rows; cy++) {
        for (int cx = 0; cx < c->plan.cols; cx++) {
            cs_tally_chunk(c, seen, cx, cy, label);
        }
    }
    for (int i = 0; i < cells; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, seen[i], label);
    }
    free(seen);
}

static void
test_the_cut_gives_every_cell_to_exactly_one_chunk(void) {
    fixture();
    for (int g = 0; g < CS_GRIDS; g++) {
        for (int oi = 0; oi < CS_OFFSETS; oi++) {
            cs_case_t c;
            char label[48];

            cs_build(&c, g, oi);
            snprintf(label, sizeof label, "%dx%d/%dx%d off %d,%d", c.plan.w, c.plan.h, c.plan.side_x, c.plan.side_y,
                     c.plan.off_x, c.plan.off_y);
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, c.plan.cols, label);
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, c.plan.rows, label);
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(SAND_CHUNKS_MAX, c.plan.cols * c.plan.rows, label);
            cs_check_partition(&c, label);
        }
    }
}

/* the order */

static void
cs_check_permutation(const cs_case_t* c, void* ctx) {
    const int n = c->plan.cols * c->plan.rows;
    uint8_t seen[SAND_CHUNKS_MAX] = {0};
    (void)ctx;

    TEST_ASSERT_EQUAL_INT_MESSAGE(n, c->order.count, c->label);
    for (int pos = 0; pos < n; pos++) {
        const int idx = c->order.at[pos];
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(n, idx, c->label);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, seen[idx], c->label);
        seen[idx] = 1;
        TEST_ASSERT_EQUAL_INT_MESSAGE(pos, c->order.rank[idx], c->label);
    }
}

static void
test_the_order_is_a_permutation_whose_rank_inverts_it(void) {
    fixture();
    cs_sweep(CS_GRIDS, cs_check_permutation, NULL);
}

static void
cs_assert_earlier(const cs_case_t* c, int idx, int nx, int ny) {
    if ((unsigned)nx >= (unsigned)c->plan.cols || (unsigned)ny >= (unsigned)c->plan.rows) {
        return;
    }
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(c->order.rank[idx], c->order.rank[ny * c->plan.cols + nx], c->label);
}

static void
cs_check_chunk_downstream(const cs_case_t* c, int cx, int cy) {
    const int idx = cy * c->plan.cols + cx;

    cs_assert_earlier(c, idx, cx + c->tx, cy + c->ty);
    if (c->tx != 0 && c->ty != 0) {
        cs_assert_earlier(c, idx, cx + c->tx, cy);
        cs_assert_earlier(c, idx, cx, cy + c->ty);
    } else if (c->ty != 0) {
        cs_assert_earlier(c, idx, cx - 1, cy + c->ty);
        cs_assert_earlier(c, idx, cx + 1, cy + c->ty);
    } else {
        cs_assert_earlier(c, idx, cx + c->tx, cy - 1);
        cs_assert_earlier(c, idx, cx + c->tx, cy + 1);
    }
}

static void
cs_check_downstream_first(const cs_case_t* c, void* ctx) {
    (void)ctx;
    for (int cy = 0; cy < c->plan.rows; cy++) {
        for (int cx = 0; cx < c->plan.cols; cx++) {
            cs_check_chunk_downstream(c, cx, cy);
        }
    }
}

static void
test_every_chunk_a_move_can_reach_runs_first(void) {
    fixture();
    cs_sweep(CS_GRIDS, cs_check_downstream_first, NULL);
}

/* two lanes */

static void
cs_run_begin(cs_run_t* r, const cs_case_t* c, bool reenter) {
    fixture();
    r->c = c;
    r->k.order = c->order;
    r->k.cols = c->plan.cols;
    r->k.rows = c->plan.rows;
    r->reenter = reenter;
    sand_chunk_sched_reset(&r->k);
}

static void
cs_assert_free_to_run(const cs_run_t* r, int cx, int cy) {
    const int idx = cy * r->c->plan.cols + cx;
    int nb[8];
    const int n = cs_neighbours(r->c->plan.cols, r->c->plan.rows, cx, cy, nb);

    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r->running[nb[i]], r->c->label);
        if (r->k.order.rank[nb[i]] < r->k.order.rank[idx]) {
            TEST_ASSERT_GREATER_THAN_UINT8_MESSAGE(0, r->ran[nb[i]], r->c->label);
        }
    }
}

static void
cs_run_fn(void* pass, int lane, int cx, int cy) {
    cs_run_t* r = (cs_run_t*)pass;
    const int idx = cy * r->c->plan.cols + cx;

    TEST_ASSERT_EQUAL_INT_MESSAGE(r->k.order.rank[idx] & 1, lane, r->c->label);
    r->running[idx] = 1;
    cs_assert_free_to_run(r, cx, cy);

    if (r->reenter && r->depth == 0) {
        r->depth = 1;
        sand_chunk_step_lane(&r->k, 1 - lane, cs_run_fn, r);
        r->depth = 0;
    }

    r->running[idx] = 0;
    r->ran[idx]++;
    r->seq[r->seq_n++] = (uint8_t)idx;
}

static int
cs_pick_lane(int which, int step, uint32_t* rnd) {
    if (which < 2) {
        return which;
    }
    if (which == 2) {
        return step & 1;
    }
    *rnd = *rnd * 1664525u + 1013904223u;
    return (int)((*rnd >> 16) & 1u);
}

/* Drives both lanes by hand, re-entering the other lane from inside the
 * chunk body so an overlap would really happen if the order permitted one. */
static void
cs_interleave(cs_run_t* r, const cs_case_t* c, int which) {
    uint32_t rnd = (which < 3) ? 0u : cs_seeds[which - 3];
    const int count = c->order.count;

    cs_run_begin(r, c, true);
    for (int step = 0; r->seq_n < count; step++) {
        const int lane = cs_pick_lane(which, step, &rnd);
        if (!sand_chunk_step_lane(&r->k, lane, cs_run_fn, r)) {
            sand_chunk_step_lane(&r->k, 1 - lane, cs_run_fn, r);
        }
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(4 * count + 64, step, c->label);
    }
}

static void
cs_check_overlap(const cs_case_t* c, void* ctx) {
    for (int which = 0; which < CS_INTERLEAVES; which++) {
        cs_interleave((cs_run_t*)ctx, c, which);
    }
}

static void
test_no_two_touching_chunks_are_ever_mid_run_together(void) {
    fixture();
    cs_sweep(CS_GRIDS, cs_check_overlap, &run);
}

static void
cs_check_complete(const cs_run_t* r) {
    const int count = r->k.order.count;

    TEST_ASSERT_EQUAL_INT_MESSAGE(count, r->seq_n, r->c->label);
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r->ran[i], r->c->label);
    }
}

static void
cs_assert_ranked_neighbours_ran_first(const cs_run_t* r, int cx, int cy, const uint8_t* when) {
    const int idx = cy * r->c->plan.cols + cx;
    int nb[8];
    const int n = cs_neighbours(r->c->plan.cols, r->c->plan.rows, cx, cy, nb);

    for (int i = 0; i < n; i++) {
        if (r->k.order.rank[nb[i]] < r->k.order.rank[idx]) {
            TEST_ASSERT_LESS_THAN_UINT8_MESSAGE(when[idx], when[nb[i]], r->c->label);
        }
    }
}

static void
cs_check_topological(const cs_run_t* r) {
    uint8_t when[SAND_CHUNKS_MAX] = {0};

    for (int p = 0; p < r->seq_n; p++) {
        when[r->seq[p]] = (uint8_t)p;
    }
    for (int cy = 0; cy < r->c->plan.rows; cy++) {
        for (int cx = 0; cx < r->c->plan.cols; cx++) {
            cs_assert_ranked_neighbours_ran_first(r, cx, cy, when);
        }
    }
}

static void
cs_check_terminates(const cs_case_t* c, void* ctx) {
    cs_run_t* r = (cs_run_t*)ctx;

    for (int which = 0; which < CS_INTERLEAVES; which++) {
        cs_interleave(r, c, which);
        cs_check_complete(r);
        cs_check_topological(r);
    }
}

static void
test_every_interleaving_runs_each_chunk_once_in_a_legal_order(void) {
    fixture();
    cs_sweep(CS_GRIDS, cs_check_terminates, &run);
}

static void
cs_check_lone_lane_gives_up(const cs_case_t* c, void* ctx) {
    cs_run_t* r = (cs_run_t*)ctx;

    cs_run_begin(r, c, false);
    sand_chunk_run_lane(&r->k, 1, 2u, cs_run_fn, r);

    TEST_ASSERT_NOT_EQUAL_UINT8_MESSAGE(0, r->k.abort, c->label);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(c->order.count, r->seq_n, c->label);

    sand_chunk_run_rest(&r->k, cs_run_fn, r);
    cs_check_complete(r);
    cs_check_topological(r);
}

static void
test_one_lane_alone_aborts_and_the_rest_finishes_the_board(void) {
    fixture();
    cs_sweep(CS_GRIDS, cs_check_lone_lane_gives_up, &run);
}

/* makespan */

static int
cs_makespan(const cs_case_t* c, const int* cost) {
    return sand_chunk_makespan(&c->order, c->plan.cols, c->plan.rows, cost);
}

static void
cs_check_uniform_makespan(const cs_case_t* c, void* ctx) {
    const int n = c->plan.cols * c->plan.rows;
    int cost[SAND_CHUNKS_MAX];
    (void)ctx;

    for (int i = 0; i < SAND_CHUNKS_MAX; i++) {
        cost[i] = 1;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(6 * n / 10 + 2, cs_makespan(c, cost), c->label);
}

static void
test_an_evenly_busy_board_keeps_both_lanes_fed(void) {
    fixture();
    cs_sweep(CS_LARGE_GRIDS, cs_check_uniform_makespan, NULL);
}

static bool
cs_on_line(const cs_case_t* c, int cx, int cy) {
    if (c->tx != 0 && c->ty != 0) {
        const int i = cs_downstream(cx, c->plan.cols, c->tx);
        const int j = cs_downstream(cy, c->plan.rows, c->ty);
        return i + j == (c->plan.cols + c->plan.rows - 2) / 2;
    }
    return (c->ty != 0) ? (cy == c->plan.rows / 2) : (cx == c->plan.cols / 2);
}

static int
cs_cost_one_line(const cs_case_t* c, int* cost) {
    int n = 0;

    for (int i = 0; i < SAND_CHUNKS_MAX; i++) {
        cost[i] = 0;
    }
    for (int cy = 0; cy < c->plan.rows; cy++) {
        for (int cx = 0; cx < c->plan.cols; cx++) {
            if (cs_on_line(c, cx, cy)) {
                cost[cy * c->plan.cols + cx] = CS_LINE_COST;
                n++;
            }
        }
    }
    return n;
}

/* A line across travel is the shape a pile or pool surface takes, and it is
 * the whole reason the order alternates within a line: ranked straight
 * along it this is one chain and the second lane idles. */
static void
cs_check_line_makespan(const cs_case_t* c, void* ctx) {
    int cost[SAND_CHUNKS_MAX];
    const int line_cost = CS_LINE_COST * cs_cost_one_line(c, cost);
    (void)ctx;

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(CS_LINE_COST, line_cost, c->label);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(6 * line_cost / 10 + 10, cs_makespan(c, cost), c->label);
}

static void
test_one_busy_line_across_travel_still_uses_both_lanes(void) {
    fixture();
    cs_sweep(CS_GRIDS, cs_check_line_makespan, NULL);
}

static void
run_sand_chunk_sched_suite(void) {
    RUN_TEST(test_the_cut_gives_every_cell_to_exactly_one_chunk);

    RUN_TEST(test_the_order_is_a_permutation_whose_rank_inverts_it);
    RUN_TEST(test_every_chunk_a_move_can_reach_runs_first);

    RUN_TEST(test_no_two_touching_chunks_are_ever_mid_run_together);
    RUN_TEST(test_every_interleaving_runs_each_chunk_once_in_a_legal_order);
    RUN_TEST(test_one_lane_alone_aborts_and_the_rest_finishes_the_board);

    RUN_TEST(test_an_evenly_busy_board_keeps_both_lanes_fed);
    RUN_TEST(test_one_busy_line_across_travel_still_uses_both_lanes);
}

SUITE_REGISTER(run_sand_chunk_sched_suite);
