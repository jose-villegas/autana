/*
 * A gravity-ordered schedule for the chunks one pass is cut into:
 * the geometry (which cells a chunk owns), one total order over the chunks
 * built from that pass's travel direction, and a two-lane runner over it.
 *
 * Ints in, tables out. No cell type, no task, no allocation, so both the
 * order and the runner are testable on a host.
 *
 * A grid updated in place with no moved flag is correct only because a
 * sweep runs against travel: a destination was already visited. Fixed
 * chunk colours cannot hold that across a border - at half of them the
 * upstream chunk runs first, and a one-cell gap opens on that line every
 * step. A total order can: a chunk waits for every 8-neighbour ahead of it
 * in the order, so any two chunks that can reach one cell are sequenced
 * and the board equals a single-threaded walk of the order whatever the
 * timing was.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SAND_CHUNKS_MAX 64

typedef struct {
    int w, h;           /* grid, cells */
    int side_x, side_y; /* chunk side per axis, cells */
    int off_x, off_y;   /* origin offset, each in [0, side of that axis) */
    int cols, rows;     /* chunk counts, cols * rows <= SAND_CHUNKS_MAX */
} sand_chunk_plan_t;

/* Cuts a w x h grid into side_x by side_y chunks whose borders sit at
 * k * side - off per axis. A non-zero offset makes the first chunk short, and
 * the last may be too. Returns false - the caller stays single-lane - when the
 * counts exceed SAND_CHUNKS_MAX or either falls below 2. */
bool sand_chunk_plan(sand_chunk_plan_t* p, int w, int h, int side_x, int side_y, int off_x, int off_y);

/* Half-open cell ranges of one chunk, clipped to the grid. */
void sand_chunk_cells(const sand_chunk_plan_t* p, int cx, int cy, int* x0, int* x1, int* y0, int* y1);

typedef struct {
    uint8_t count;
    uint8_t at[SAND_CHUNKS_MAX];   /* position -> chunk index cy * cols + cx */
    uint8_t rank[SAND_CHUNKS_MAX]; /* chunk index -> position */
} sand_chunk_order_t;

/* Orders the chunks for a pass travelling (tx, ty), each in {-1, 0, +1} and
 * not both zero: most downstream first, so a chunk's destination side is
 * already settled by the time it runs.
 *
 * Within one line across the travel axis, `phase` picks which parity of the
 * line index goes first. No move crosses a border inside such a line, so
 * alternating its chunks is what lets a pile or pool surface - which lies
 * across gravity - occupy both lanes instead of becoming one chain. */
void sand_chunk_order(sand_chunk_order_t* o, int cols, int rows, int tx, int ty, unsigned phase);

typedef struct {
    sand_chunk_order_t order;
    int cols, rows;
    volatile uint8_t done[SAND_CHUNKS_MAX]; /* by chunk index; single writer each */
    volatile uint8_t abort;
    uint8_t cursor[2]; /* next position per lane; written by that lane only */
} sand_chunk_sched_t;

/* A chunk's lane is its position's parity, never the thread that happens to
 * reach it, so lane-keyed private state gives one lane the same bytes it
 * gives two. */
typedef void (*sand_chunk_fn_t)(void* pass, int lane, int cx, int cy);

/* Call once `order`, `cols` and `rows` are set. */
void sand_chunk_sched_reset(sand_chunk_sched_t* k);

/* Whether every 8-neighbour ranked ahead of the chunk at `pos` is done. */
bool sand_chunk_ready(const sand_chunk_sched_t* k, int pos);

/* Runs the lane's next chunk if it is ready, marking it done only after
 * `fn` returns. Never blocks; false means it did nothing. */
bool sand_chunk_step_lane(sand_chunk_sched_t* k, int lane, sand_chunk_fn_t fn, void* pass);

/* Steps the lane to exhaustion, re-polling a chunk that is not ready up to
 * `spin_limit` times - a bounded spin on volatile bytes, no sleep and no OS
 * call. Past the limit, or on finding `abort` already set, it sets `abort`
 * and returns with work left. A lane only ever waits on positions earlier
 * in the order, so two lanes cannot deadlock. */
void sand_chunk_run_lane(sand_chunk_sched_t* k, int lane, unsigned spin_limit, sand_chunk_fn_t fn, void* pass);

/* Walks the whole order on this thread and runs whatever is not done yet.
 * Requires that no lane is still running. On an untouched schedule this is
 * the single-threaded walk every interleaving has to match. */
void sand_chunk_run_rest(sand_chunk_sched_t* k, sand_chunk_fn_t fn, void* pass);
