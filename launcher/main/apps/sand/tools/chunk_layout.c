/*
 * Ranks chunk layouts on the host, so the QEMU sweep that follows has a
 * handful of sides to measure per quality instead of a cross-product.
 *
 * For every quality grid, scene, gravity class and legal side pair it runs the
 * real split path on one lane (SAND_CHUNK_PASS_SOLO), charges each chunk the
 * cells its passes actually dispatched, and feeds those to the scheduler's own
 * two-lane span (sand_chunk_makespan()). Balance is makespan / total work:
 * 0.50 is two lanes never idle, 1.00 is one chain.
 *
 * WORK IS NOT TIME. A cell a pass dispatched costs whatever its material and
 * neighbourhood say, and this counts it as one. The output ranks layouts by
 * how evenly a board's work divides; what a second core buys is measured under
 * QEMU afterwards, and on the board after that.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sand.h"
#include "sand_priv.h"

#define CL_SAND       SAND_FIRST_SHADE
#define CL_STONE      CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT)
#define CL_WATER      CELL_MAKE(MAT_WATER, MASS_MAX)

#define STEPS         16
#define SETTLE_STEPS  200
#define AXIS_SAMPLES  6
#define SHORTLIST_MAX 4

typedef struct {
    const char* name;
    int w, h;
} quality_t;

/* The five the boot menu offers, at 368 x 448 - the same grids
 * suite_sand_chunk_sched.c sweeps. */
static const quality_t qualities[] = {
    {"ULTRA", 184, 224}, {"HIGH", 122, 149}, {"NORMAL", 92, 112}, {"LOW", 61, 74}, {"VERY LOW", 46, 56},
};

typedef struct {
    const char* name;
    int gx, gy;
} gravity_t;

/* Landscape first: it is the shipping orientation. The diagonal is the one
 * class whose chunk order is a wave rather than a line sweep. */
static const gravity_t gravities[] = {
    {"landscape", 1000, 0},
    {"portrait", 0, 1000},
    {"diagonal", 700, 700},
};

typedef struct {
    sand_t s;
    uint8_t* cells;
    uint8_t* blocks;
    uint8_t* stamps;
    void* scratch;
} board_t;

static void
board_open(board_t* b, int w, int h, uint32_t seed) {
    b->cells = malloc((size_t)w * (size_t)h);
    b->blocks =
        malloc((size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    b->stamps = malloc(sand_step_stamp_bytes(w, h));
    b->scratch = malloc(sand_lane_scratch_bytes(w, h));
    if (b->cells == NULL || b->blocks == NULL || b->stamps == NULL || b->scratch == NULL) {
        fprintf(stderr, "chunk_layout: out of memory at %dx%d\n", w, h);
        exit(1);
    }
    sand_init(&b->s, b->cells, w, h, seed);
    sand_enable_sleeping(&b->s, b->blocks);
    sand_enable_step_stamps(&b->s, b->stamps);
    sand_enable_lane_scratch(&b->s, b->scratch);
}

static void
board_close(board_t* b) {
    free(b->scratch);
    free(b->stamps);
    free(b->blocks);
    free(b->cells);
}

/* --- scenes ---------------------------------------------------------------
 *
 * The first three mirror what the frame-budget suite measures, rebuilt here
 * because its own builders are fixed at one grid. The last two are the shapes
 * a layout is most likely to divide badly: work confined to one line.
 */

static void
fill(sand_t* s, int x0, int x1, int y0, int y1, cell_t c) {
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            sand_set(s, x, y, c);
        }
    }
}

static void
build_mixed_flip(board_t* b, int w, int h) {
    board_open(b, w, h, 17u);

    const int sand_x1 = (w * 3) / 10;
    const int water_x0 = w - (w * 3) / 10;
    fill(&b->s, 0, sand_x1, h / 2, h, CL_SAND);
    fill(&b->s, water_x0, w, h / 2, h, CL_WATER);
    for (int y = 0; y < h; y++) {
        const int off = (y * (water_x0 - sand_x1 - 1)) / (h - 1);
        sand_set(&b->s, sand_x1 + off, y, CL_STONE);
        sand_set(&b->s, water_x0 - 1 - off, y, CL_STONE);
    }
    for (int i = 0; i < SETTLE_STEPS; i++) {
        sand_step(&b->s, 0, 1000, 0);
    }
}

static void
build_water(board_t* b, int w, int h) {
    board_open(b, w, h, 11u);
    fill(&b->s, w / 4, (w * 3) / 4, 0, h / 2, CL_WATER);
}

static void
build_sand_only(board_t* b, int w, int h) {
    board_open(b, w, h, 99u);
    for (int y = 0; y < h / 2; y++) {
        for (int x = 0; x < w; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(&b->s, x, y, CL_SAND);
            }
        }
    }
}

/* Poured and left until only its surface still moves: most chunks asleep,
 * and the ones that are not lie along one line across gravity. */
static void
build_settling_pile(board_t* b, int w, int h) {
    board_open(b, w, h, 5u);
    fill(&b->s, w / 4, (w * 3) / 4, h / 3, h, CL_SAND);
    for (int i = 0; i < SETTLE_STEPS; i++) {
        sand_step(&b->s, 0, 1000, 0);
    }
}

/* A basin of water whose surface is uneven, so cross-flow has somewhere to
 * move mass on every ray and the work is a band, not a block. */
static void
build_levelling_pool(board_t* b, int w, int h) {
    board_open(b, w, h, 7u);
    fill(&b->s, 1, w - 1, (h * 2) / 3, h, CL_WATER);
    fill(&b->s, 0, 1, 0, h, CL_STONE);
    fill(&b->s, w - 1, w, 0, h, CL_STONE);
    fill(&b->s, 0, w, h - 1, h, CL_STONE);
    fill(&b->s, w / 3, (w * 2) / 3, h / 2, (h * 2) / 3, CL_WATER);
    for (int i = 0; i < SETTLE_STEPS / 4; i++) {
        sand_step(&b->s, 0, 1000, 0);
    }
}

typedef void (*build_fn)(board_t*, int, int);

typedef struct {
    const char* name;
    build_fn build;
    int warm; /* steps under the measured gravity before counting starts */
} scene_t;

/* The first three are measured on the transient - a board still collapsing,
 * or turned onto a new down. The last two are warmed in the orientation they
 * are measured in, because what they are for is a board whose work has
 * already shrunk to a surface. */
static const scene_t scenes[] = {
    {"mixed-flip", build_mixed_flip, 0},
    {"water", build_water, 0},
    {"sand-only", build_sand_only, 0},
    {"settling-pile", build_settling_pile, SETTLE_STEPS},
    {"levelling-pool", build_levelling_pool, SETTLE_STEPS},
};

#define QUALITIES ((int)(sizeof qualities / sizeof qualities[0]))
#define SCENES    ((int)(sizeof scenes / sizeof scenes[0]))
#define GRAVITIES ((int)(sizeof gravities / sizeof gravities[0]))

/* --- one cell of the sweep ------------------------------------------------ */

typedef struct {
    int chunks;
    long long work;
    long long makespan;
    long long awake;
    int steps;
} result_t;

/* long long throughout: a quality's work over sixteen steps runs to millions
 * of cells, and this host's long is 32 bits. */
static long long
sum_chunk_work(int chunks, int* cost) {
    long long total = 0;

    for (int i = 0; i < chunks; i++) {
        cost[i] = (int)sand_chunk_work[i];
        total += cost[i];
    }
    return total;
}

static void
measure(const quality_t* q, const scene_t* sc, const gravity_t* g, int side_x, int side_y, result_t* out) {
    board_t b;
    sand_chunk_plan_t plan;

    memset(out, 0, sizeof *out);
    if (!sand_chunk_plan(&plan, q->w, q->h, side_x, side_y, 0, 0)) {
        return;
    }
    out->chunks = plan.cols * plan.rows;

    sc->build(&b, q->w, q->h);
    if (!sand_chunk_side_for_test(side_x, side_y)) {
        fprintf(stderr, "chunk_layout: %dx%d refused\n", side_x, side_y);
        exit(1);
    }
    sand_set_two_core_step(true);
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
    for (int i = 0; i < sc->warm; i++) {
        sand_step(&b.s, g->gx, g->gy, 0);
    }
    sand_chunk_work_enable(true);

    for (int i = 0; i < STEPS; i++) {
        int cost[SAND_CHUNKS_MAX];
        sand_chunk_order_t order;
        int dx, dy;

        memset(sand_chunk_work, 0, sizeof sand_chunk_work);
        sand_step(&b.s, g->gx, g->gy, 0);

        const long long work = sum_chunk_work(out->chunks, cost);
        if (work == 0) {
            continue;
        }
        /* The sweep's own travel and phase: the other passes run their own
         * directions over the same cut, and this charges all of their work to
         * the order of the one that moves most of it. */
        sand_gravity_direction(g->gx, g->gy, &dx, &dy);
        sand_chunk_order(&order, plan.cols, plan.rows, dx, dy, b.s.step_phase);
        out->makespan += sand_chunk_makespan(&order, plan.cols, plan.rows, cost);
        out->work += work;
        for (int c = 0; c < out->chunks; c++) {
            out->awake += (cost[c] > 0);
        }
        out->steps++;
    }

    sand_chunk_work_enable(false);
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);
    sand_set_two_core_step(false);
    (void)sand_chunk_side_for_test(0, 0);
    board_close(&b);
}

/* --- candidate sides ------------------------------------------------------ */

/* Keeps `out` ascending and free of duplicates. */
static int
insert_side(int* out, int n, int side) {
    int at = 0;

    while (at < n && out[at] < side) {
        at++;
    }
    if (at < n && out[at] == side) {
        return n;
    }
    for (int i = n; i > at; i--) {
        out[i] = out[i - 1];
    }
    out[at] = side;
    return n + 1;
}

/* Every legal side from the floor to half the axis, thinned to AXIS_SAMPLES so
 * a quality's cross-product stays small, plus the side that ships so the table
 * reads against today rather than only against itself. Whether a pair plans at
 * all is sand_chunk_plan()'s answer, taken per pair rather than per axis. */
static int
axis_sides(int extent, int rule, int* out) {
    const int hi = extent / 2;
    int n = 0;

    if (hi < SAND_CHUNK_SIDE_MIN) {
        out[n++] = SAND_CHUNK_SIDE_MIN;
        return n;
    }
    for (int i = 0; i < AXIS_SAMPLES; i++) {
        n = insert_side(out, n, SAND_CHUNK_SIDE_MIN + ((hi - SAND_CHUNK_SIDE_MIN) * i) / (AXIS_SAMPLES - 1));
    }
    if (rule <= hi && rule >= SAND_CHUNK_SIDE_MIN) {
        n = insert_side(out, n, rule);
    }
    return n;
}

/* sand_chunk_side_rule() takes a board; this is the same arithmetic on a
 * grid size, so the table can name the shipped side without building one. */
static int
rule_side(int w, int h) {
    const int cells_per_chunk = (w * h) / SAND_CHUNK_TARGET_CELLS_DIVISOR;
    int side = 1;

    while (side <= cells_per_chunk / side) {
        side++;
    }
    side--;
    return side < SAND_CHUNK_SIDE_MIN ? SAND_CHUNK_SIDE_MIN : side;
}

/* --- the report ----------------------------------------------------------- */

typedef struct {
    int side_x, side_y;
    double balance; /* makespan / work: 0.5 is two lanes never idle */
    double landscape;
    int chunks;
} layout_t;

/* Landscape counts double: it is the orientation the app ships in, but a
 * layout that only divides well there is not a layout anyone wants. */
static double
rank_of(const layout_t* l) {
    return 2.0 * l->landscape + l->balance;
}

static int
by_landscape_then_balance(const void* a, const void* b) {
    const double ra = rank_of(a);
    const double rb = rank_of(b);

    if (ra != rb) {
        return (ra < rb) ? -1 : 1;
    }
    return 0;
}

static double
balance_of(long long span, long long work) {
    return work > 0 ? (double)span / (double)work : 1.0;
}

/* Every scene and gravity class at one side pair: a table row each, and the
 * pair's own aggregate. False when the grid cannot be cut that way at all. */
static bool
measure_layout(const quality_t* q, int side_x, int side_y, layout_t* out) {
    long long work = 0, span = 0, land_work = 0, land_span = 0;
    int chunks = 0;

    for (int si = 0; si < SCENES; si++) {
        for (int gi = 0; gi < GRAVITIES; gi++) {
            result_t r;

            measure(q, &scenes[si], &gravities[gi], side_x, side_y, &r);
            if (r.steps == 0) {
                continue;
            }
            chunks = r.chunks;
            printf("| %dx%d | %d | %s | %s | %lld | %lld | %.3f | %.1f |\n", side_x, side_y, r.chunks, scenes[si].name,
                   gravities[gi].name, r.work, r.makespan, balance_of(r.makespan, r.work),
                   (double)r.awake / (double)r.steps);
            work += r.work;
            span += r.makespan;
            if (gi == 0) {
                land_work += r.work;
                land_span += r.makespan;
            }
        }
    }
    if (chunks == 0) {
        return false;
    }
    *out = (layout_t){.side_x = side_x,
                      .side_y = side_y,
                      .chunks = chunks,
                      .balance = balance_of(span, work),
                      .landscape = balance_of(land_span, land_work)};
    return true;
}

static void
print_shortlist(const quality_t* q, int rule, const layout_t* found, int n) {
    printf("\nShortlist for %s (landscape weighted double):\n\n", q->name);
    if (n == 0) {
        printf("- no side pair this grid can be cut by plans at all\n");
        return;
    }
    for (int i = 0; i < n && i < SHORTLIST_MAX; i++) {
        printf("- `%dx%d` - %d chunks, landscape %.3f, all gravities %.3f\n", found[i].side_x, found[i].side_y,
               found[i].chunks, found[i].landscape, found[i].balance);
    }
    for (int i = 0; i < n; i++) {
        if (found[i].side_x == rule && found[i].side_y == rule) {
            printf("\nShipped `%dx%d` for comparison: %d chunks, landscape %.3f, all gravities %.3f, "
                   "ranked %d of %d\n",
                   rule, rule, found[i].chunks, found[i].landscape, found[i].balance, i + 1, n);
            return;
        }
    }
    printf("\nShipped `%dx%d` is not a cut this grid takes - it falls back to one lane.\n", rule, rule);
}

static void
report_quality(const quality_t* q) {
    const int rule = rule_side(q->w, q->h);
    int sx[AXIS_SAMPLES + 1], sy[AXIS_SAMPLES + 1];
    const int nx = axis_sides(q->w, rule, sx);
    const int ny = axis_sides(q->h, rule, sy);
    layout_t found[(AXIS_SAMPLES + 1) * (AXIS_SAMPLES + 1)];
    int n_found = 0;

    printf("\n## %s - %d x %d cells, shipped side %d\n\n", q->name, q->w, q->h, rule);
    printf("| side | chunks | scene | gravity | work | makespan | balance | awake/step |\n");
    printf("|---|---|---|---|---|---|---|---|\n");

    for (int ix = 0; ix < nx; ix++) {
        for (int iy = 0; iy < ny; iy++) {
            n_found += measure_layout(q, sx[ix], sy[iy], &found[n_found]);
        }
    }
    qsort(found, (size_t)n_found, sizeof found[0], by_landscape_then_balance);
    print_shortlist(q, rule, found, n_found);
}

int
main(void) {
    printf("# Chunk layout pre-filter\n\n");
    printf("Work is cells a pass dispatched, not time: QEMU ranks the shortlist, "
           "the board confirms it. %d steps per cell, one lane, chunk floor %d.\n",
           STEPS, SAND_CHUNK_SIDE_MIN);

    for (int i = 0; i < QUALITIES; i++) {
        report_quality(&qualities[i]);
    }
    return 0;
}
