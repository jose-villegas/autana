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

#include "apps/sand/tests/suite_sand_scenes.h"
#include "sand.h"
#include "sand_priv.h"

#define STEPS         16
#define AXIS_SAMPLES  6
#define SHORTLIST_MAX 4

/* Unity comes in with the shared scene builders and wants both. */
void
setUp(void) {}

void
tearDown(void) {}

typedef struct {
    const char* name;
    int w, h;
} quality_t;

/* The five the options screen offers, at 368 x 448 - the same grids
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

/*
 * scenes
 *
 * suite_sand_scenes.c's layout set, so this tool and the emulated sweep rank
 * and measure the same five boards.
 */

typedef int (*build_fn)(sand_t*);

typedef struct {
    const char* name;
    build_fn build;
} scene_t;

static const scene_t scenes[] = {
    {"mixed-flip", build_layout_mixed_flip_scene},         {"water", build_layout_water_scene},
    {"sand-only", build_layout_sand_only_scene},           {"settling-pile", build_layout_settling_pile_scene},
    {"levelling-pool", build_layout_levelling_pool_scene},
};

#define QUALITIES ((int)(sizeof qualities / sizeof qualities[0]))
#define SCENES    ((int)(sizeof scenes / sizeof scenes[0]))
#define GRAVITIES ((int)(sizeof gravities / sizeof gravities[0]))

/* one cell of the sweep */

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

    board_open(&b, q->w, q->h, 11u);
    const int warm = sc->build(&b.s);
    if (!sand_chunk_side_for_test(side_x, side_y)) {
        fprintf(stderr, "chunk_layout: %dx%d refused\n", side_x, side_y);
        exit(1);
    }
    sand_set_two_core_step(true);
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
    for (int i = 0; i < warm; i++) {
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

/* candidate sides */

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
 * a quality's cross-product stays small, plus the sides that ship so the table
 * reads against today rather than only against itself. Whether a pair plans at
 * all is sand_chunk_plan()'s answer, taken per pair rather than per axis. */
static int
axis_sides(int extent, const int* keep, int n_keep, int* out) {
    const int hi = extent / 2;
    int n = 0;

    if (hi < SAND_CHUNK_SIDE_MIN) {
        out[n++] = SAND_CHUNK_SIDE_MIN;
        return n;
    }
    for (int i = 0; i < AXIS_SAMPLES; i++) {
        n = insert_side(out, n, SAND_CHUNK_SIDE_MIN + ((hi - SAND_CHUNK_SIDE_MIN) * i) / (AXIS_SAMPLES - 1));
    }
    for (int i = 0; i < n_keep; i++) {
        if (keep[i] <= hi && keep[i] >= SAND_CHUNK_SIDE_MIN) {
            n = insert_side(out, n, keep[i]);
        }
    }
    return n;
}

/* the report */

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
print_shipped_rank(const char* travel, int side_x, int side_y, const layout_t* found, int n) {
    for (int i = 0; i < n; i++) {
        if (found[i].side_x == side_x && found[i].side_y == side_y) {
            printf("\nShipped along %s, `%dx%d`: %d chunks, landscape %.3f, all gravities %.3f, ranked %d of %d\n",
                   travel, side_x, side_y, found[i].chunks, found[i].landscape, found[i].balance, i + 1, n);
            return;
        }
    }
    printf("\nShipped along %s, `%dx%d`, is not a cut this grid takes - it falls back to one lane.\n", travel, side_x,
           side_y);
}

static void
print_shortlist(const quality_t* q, const int* ship, const layout_t* found, int n) {
    printf("\nShortlist for %s (landscape weighted double):\n\n", q->name);
    if (n == 0) {
        printf("- no side pair this grid can be cut by plans at all\n");
        return;
    }
    for (int i = 0; i < n && i < SHORTLIST_MAX; i++) {
        printf("- `%dx%d` - %d chunks, landscape %.3f, all gravities %.3f\n", found[i].side_x, found[i].side_y,
               found[i].chunks, found[i].landscape, found[i].balance);
    }
    print_shipped_rank("x", ship[0], ship[1], found, n);
    print_shipped_rank("anything else", ship[2], ship[3], found, n);
}

static void
report_quality(const quality_t* q) {
    int ship[4];
    sand_chunk_table_sides(q->w, q->h, SAND_CHUNK_TRAVEL_X, &ship[0], &ship[1]);
    sand_chunk_table_sides(q->w, q->h, SAND_CHUNK_TRAVEL_OTHER, &ship[2], &ship[3]);

    const int keep_x[] = {ship[0], ship[2]};
    const int keep_y[] = {ship[1], ship[3]};
    int sx[AXIS_SAMPLES + 2], sy[AXIS_SAMPLES + 2];
    const int nx = axis_sides(q->w, keep_x, 2, sx);
    const int ny = axis_sides(q->h, keep_y, 2, sy);
    layout_t found[(AXIS_SAMPLES + 2) * (AXIS_SAMPLES + 2)];
    int n_found = 0;

    printf("\n## %s - %d x %d cells, shipped %dx%d along x, %dx%d otherwise\n\n", q->name, q->w, q->h, ship[0], ship[1],
           ship[2], ship[3]);
    printf("| side | chunks | scene | gravity | work | makespan | balance | awake/step |\n");
    printf("|---|---|---|---|---|---|---|---|\n");

    for (int ix = 0; ix < nx; ix++) {
        for (int iy = 0; iy < ny; iy++) {
            n_found += measure_layout(q, sx[ix], sy[iy], &found[n_found]);
        }
    }
    qsort(found, (size_t)n_found, sizeof found[0], by_landscape_then_balance);
    print_shortlist(q, ship, found, n_found);
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
