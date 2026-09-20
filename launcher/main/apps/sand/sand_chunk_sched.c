#include "sand_chunk_sched.h"

static int
chunk_count(int extent, int side, int off) {
    return (extent + off + side - 1) / side;
}

bool
sand_chunk_plan(sand_chunk_plan_t* p, int w, int h, int side_x, int side_y, int off_x, int off_y) {
    if (w < 1 || h < 1 || side_x < 1 || side_y < 1) {
        return false;
    }
    if (off_x < 0 || off_x >= side_x || off_y < 0 || off_y >= side_y) {
        return false;
    }

    const int cols = chunk_count(w, side_x, off_x);
    const int rows = chunk_count(h, side_y, off_y);
    if (cols < 2 || rows < 2 || cols * rows > SAND_CHUNKS_MAX) {
        return false;
    }

    p->w = w;
    p->h = h;
    p->side_x = side_x;
    p->side_y = side_y;
    p->off_x = off_x;
    p->off_y = off_y;
    p->cols = cols;
    p->rows = rows;
    return true;
}

static void
chunk_span(int c, int side, int off, int extent, int* a, int* b) {
    int lo = c * side - off;
    int hi = lo + side;

    if (lo < 0) {
        lo = 0;
    }
    if (hi > extent) {
        hi = extent;
    }
    *a = lo;
    *b = hi;
}

void
sand_chunk_cells(const sand_chunk_plan_t* p, int cx, int cy, int* x0, int* x1, int* y0, int* y1) {
    chunk_span(cx, p->side_x, p->off_x, p->w, x0, x1);
    chunk_span(cy, p->side_y, p->off_y, p->h, y0, y1);
}

/* Turns an index counted from an axis's downstream end into a coordinate. */
static int
from_downstream(int i, int n, int t) {
    return (t > 0) ? (n - 1 - i) : i;
}

static void
order_push(sand_chunk_order_t* o, int cols, int cx, int cy) {
    const uint8_t idx = (uint8_t)(cy * cols + cx);

    o->rank[idx] = o->count;
    o->at[o->count] = idx;
    o->count++;
}

static void
order_push_in_line(sand_chunk_order_t* o, int cols, int line, int j, bool j_is_x) {
    if (j_is_x) {
        order_push(o, cols, j, line);
    } else {
        order_push(o, cols, line, j);
    }
}

static void
order_axis_line(sand_chunk_order_t* o, int cols, int line, int span, bool j_is_x, unsigned phase) {
    for (unsigned half = 0; half < 2u; half++) {
        const int want = (int)((phase ^ half) & 1u);
        for (int j = 0; j < span; j++) {
            if ((j & 1) == want) {
                order_push_in_line(o, cols, line, j, j_is_x);
            }
        }
    }
}

static void
order_axis(sand_chunk_order_t* o, int cols, int rows, int tx, int ty, unsigned phase) {
    const bool along_y = (ty != 0);
    const int lines = along_y ? rows : cols;
    const int span = along_y ? cols : rows;
    const int t = along_y ? ty : tx;

    for (int i = 0; i < lines; i++) {
        order_axis_line(o, cols, from_downstream(i, lines, t), span, along_y, phase);
    }
}

static void
order_diag_wave(sand_chunk_order_t* o, int cols, int rows, int tx, int ty, int wave) {
    for (int par = 0; par < 2; par++) {
        for (int i = 0; i < cols; i++) {
            const int j = wave - i;
            if ((i & 1) != par || j < 0 || j >= rows) {
                continue;
            }
            order_push(o, cols, from_downstream(i, cols, tx), from_downstream(j, rows, ty));
        }
    }
}

static void
order_diag(sand_chunk_order_t* o, int cols, int rows, int tx, int ty) {
    for (int wave = 0; wave <= cols + rows - 2; wave++) {
        order_diag_wave(o, cols, rows, tx, ty, wave);
    }
}

void
sand_chunk_order(sand_chunk_order_t* o, int cols, int rows, int tx, int ty, unsigned phase) {
    o->count = 0;
    if (tx != 0 && ty != 0) {
        order_diag(o, cols, rows, tx, ty);
    } else {
        order_axis(o, cols, rows, tx, ty, phase);
    }
}

void
sand_chunk_sched_reset(sand_chunk_sched_t* k) {
    for (int i = 0; i < SAND_CHUNKS_MAX; i++) {
        k->done[i] = 0;
    }
    k->abort = 0;
    k->cursor[0] = 0;
    k->cursor[1] = 1;
}

static const int8_t chunk_ring[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

bool
sand_chunk_ready(const sand_chunk_sched_t* k, int pos) {
    const int idx = k->order.at[pos];
    const int cx = idx % k->cols;
    const int cy = idx / k->cols;

    for (int n = 0; n < 8; n++) {
        const int nx = cx + chunk_ring[n][0];
        const int ny = cy + chunk_ring[n][1];
        if ((unsigned)nx >= (unsigned)k->cols || (unsigned)ny >= (unsigned)k->rows) {
            continue;
        }
        const int nidx = ny * k->cols + nx;
        if (k->order.rank[nidx] < pos && !k->done[nidx]) {
            return false;
        }
    }
    return true;
}

bool
sand_chunk_step_lane(sand_chunk_sched_t* k, int lane, sand_chunk_fn_t fn, void* pass) {
    const int pos = k->cursor[lane];

    if (pos >= k->order.count || !sand_chunk_ready(k, pos)) {
        return false;
    }

    const int idx = k->order.at[pos];
    fn(pass, lane, idx % k->cols, idx / k->cols);
    k->done[idx] = 1;
    k->cursor[lane] = (uint8_t)(pos + 2);
    return true;
}

void
sand_chunk_run_lane(sand_chunk_sched_t* k, int lane, unsigned spin_limit, sand_chunk_fn_t fn, void* pass) {
    unsigned spins = 0;

    while (k->cursor[lane] < k->order.count && !k->abort) {
        if (sand_chunk_step_lane(k, lane, fn, pass)) {
            spins = 0;
            continue;
        }
        if (spins >= spin_limit) {
            k->abort = 1;
            return;
        }
        spins++;
    }
}

int
sand_chunk_makespan(const sand_chunk_order_t* o, int cols, int rows, const int* cost) {
    int finish[SAND_CHUNKS_MAX] = {0};
    int lane_idle[2] = {0, 0};
    int span = 0;

    for (int pos = 0; pos < o->count; pos++) {
        const int idx = o->at[pos];
        const int cx = idx % cols;
        const int cy = idx / cols;
        int start = lane_idle[pos & 1];

        for (int n = 0; n < 8; n++) {
            const int nx = cx + chunk_ring[n][0];
            const int ny = cy + chunk_ring[n][1];
            if ((unsigned)nx >= (unsigned)cols || (unsigned)ny >= (unsigned)rows) {
                continue;
            }
            const int r = o->rank[ny * cols + nx];
            if (r < pos && finish[r] > start) {
                start = finish[r];
            }
        }
        finish[pos] = start + cost[idx];
        lane_idle[pos & 1] = finish[pos];
        span = (finish[pos] > span) ? finish[pos] : span;
    }
    return span;
}

void
sand_chunk_run_rest(sand_chunk_sched_t* k, sand_chunk_fn_t fn, void* pass) {
    for (int pos = 0; pos < k->order.count; pos++) {
        const int idx = k->order.at[pos];
        if (k->done[idx]) {
            continue;
        }
        fn(pass, pos & 1, idx % k->cols, idx / k->cols);
        k->done[idx] = 1;
    }
}
