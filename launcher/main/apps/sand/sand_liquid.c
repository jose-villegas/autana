/*
 * sand_liquid - everything about a liquid that is not the powder sweep.
 *
 * sand_step(), in sand.c, moves every grain the same way whatever it is made
 * of: try to fall, then try the two slides. A liquid needs that exact
 * treatment too - it is still gravity-ward, still bound by the sweep's
 * no-double-move guarantee - so move_liquid_grain() is called FROM inside
 * that sweep rather than living here as a separate pass.
 *
 * Everything else in a liquid's behaviour is NOT gravity-ward, and so cannot
 * safely live in that sweep at all - see the comment above equalise_liquids()
 * for why. sand_step_liquids() is the one thing sand_step() calls after its
 * sweep finishes: cross-flow levelling.
 */

#include "sand_priv.h"

#include <stdlib.h>

#ifdef DEVICE_BUILD
#include "esp_timer.h"
#endif

#include "sand_liquid_move.h"
#include "util/fixed.h"

/* See liquid_mask() in sand_priv.h */

/* See sand_priv.h. */
unsigned sand_liquid_moves;
unsigned sand_liquid_crossflow_probes;

#define LIQUID_STRIPE_H       SAND_BLOCK_H
#define LIQUID_SPLIT_MIN_ROWS (4 * LIQUID_STRIPE_H)

typedef struct {
    unsigned moves, probes;
    const uint8_t* block_read;
    uint8_t* arrivals;
    bool guard;
} liquid_work_t;

static inline size_t
arrival_byte(const sand_t* s, int x, int y) {
    return (size_t)y * (((size_t)s->w + 7) / 8) + (unsigned)x / 8;
}

/* Everything that is NOT gravity-ward, and so cannot live in that sweep. */

/* WHY A SEPARATE PASS: the main sweep guarantees no double-move sweeping
 * gravity-ward; tilted gravity pins both axes, so only ONE cross-flow
 * direction is safe. Without this, water crosses a slope one way and
 * never back - a tilted pool walks into the low corner and stays. A
 * separate pass alternates direction each step, only moving liquid
 * ACROSS flow, so it cannot disturb the main sweep. Falling water does
 * not spread: a cell able to fall THIS step leaves cross-flow nothing
 * to decide. */
static inline bool
has_room_below(const uint8_t* below_row, int w, int fx, uint8_t id) {
    if (below_row == NULL || (unsigned)fx >= (unsigned)w) {
        return false;
    }
    const cell_t below = below_row[fx];
    return CELL_IS_EMPTY(below) || (CELL_MATERIAL(below) == id && CELL_VARIANT(below) < MASS_MAX);
}

/* `there < MASS_MAX` must stay part of the test, not just the level
 * comparison: once a bias is in play an EQUAL-mass neighbour can
 * legitimately read as lower, so "is it lower" alone would send every
 * interior cell of a full pool on a full sight walk - the exact cost
 * this function exists to avoid. */
static inline bool
neighbour_is_lower(const uint8_t* n_row, int w, int nx, uint8_t id, int mass, int bias_q8) {
    if (n_row == NULL || (unsigned)nx >= (unsigned)w) {
        return false;
    }
    const cell_t n = n_row[nx];
    /* Rejects OTHER material as full using same comparison. One branch for
     * both. */
    const int there = CELL_IS_EMPTY(n) ? 0 : (CELL_MATERIAL(n) == id ? CELL_VARIANT(n) : MASS_MAX);
    return there < MASS_MAX && (there << 8) - bias_q8 < (mass << 8);
}

/* Shallowest cell by level, steps away, 1/256 mass drop. Flow stops at
 * different liquid. */
static inline int
find_shallowest(const sand_t* s, int x, int y, int px, int py, int sight, uint8_t id, int mass, int bias_q8,
                int* lowest, int* at, liquid_work_t* work) {
    const int mine = mass << 8;
    int best = mine;
    int carried = 0;
    int low = mass;
    int k_at = 0;

    for (int k = 1; k <= sight; k++) {
        const int sx = x + px * k;
        const int sy = y + py * k;

        if ((unsigned)sx >= (unsigned)s->w || (unsigned)sy >= (unsigned)s->h) {
            break;
        }
        work->probes++;
        const cell_t o = s->cells[(size_t)sy * (size_t)s->w + (size_t)sx];
        int there;

        if (CELL_IS_EMPTY(o)) {
            there = 0;
        } else if (CELL_MATERIAL(o) == id) {
            there = CELL_VARIANT(o);
        } else {
            break;
        }

        carried -= bias_q8;
        const int level = (there << 8) + carried;
        if (level < best) {
            best = level;
            low = there;
            k_at = k;
        }
        if (there == 0) {
            break; /* nothing is lower than dry */
        }
    }

    *lowest = low;
    *at = k_at;
    return mine - best;
}

/* Returns transfer status and row confinement; sets `*touched_x` if confined.
 * BOTH PROBE ROWS ARE PASSED IN, not derived: the caller resolved them once
 * for the whole row walk. Deriving them here cost a multiply and a reload of
 * s->w, s->h and s->cells per probe, which the compiler cannot hoist because
 * the transfer below writes through s->cells and may alias those fields. */
static inline bool
equalise_one_cell(sand_t* s, uint8_t* row, int x, int y, const uint8_t* below_row, const uint8_t* n_row, int w, int px,
                  int py, int dx, int sight, uint8_t id, int mass, int bias_q8, bool* stayed_in_row, int* touched_x,
                  liquid_work_t* work) {
    if (has_room_below(below_row, w, x + dx, id)) {
        return false;
    }
    if (!neighbour_is_lower(n_row, w, x + px, id, mass, bias_q8)) {
        return false;
    }
    if (s->may_have_viscous_liquid && !liquid_may_move(s, x, y, id)) {
        return false; /* viscosity affects levelling; syrupy liquid would
                         * level instantly sideways, resembling "runny" */
    }

    int lowest, at;
    const int drop_q8 = find_shallowest(s, x, y, px, py, sight, id, mass, bias_q8, &lowest, &at, work);

    /* Avoids trading mass imbalance - `>> 9` halves and converts q8 to whole
     * mass. */
    int give = drop_q8 >> 9;
    if (__builtin_expect(at == 0 || give <= 0, 1)) {
        return false;
    }
    /* Two clamps the old rule never needed: it only ever moved half of a
     * difference in MASS, which could not overrun either end. A level
     * difference can be far larger than the mass on hand or the room at
     * the far end, so both ends are pinned here. */
    if (__builtin_expect(give > MASS_MAX - lowest, 0)) {
        give = MASS_MAX - lowest;
    }
    if (__builtin_expect(give > mass, 0)) {
        give = mass;
    }
    if (__builtin_expect(give <= 0, 0)) {
        return false;
    }

    const int tx = x + px * at;
    const int ty = y + py * at;

    const bool was_empty = pour_into(&s->cells[(size_t)ty * (size_t)w + (size_t)tx], id, give);
    row[x] = (mass - give > 0) ? CELL_MAKE(id, mass - give) : CELL_EMPTY;
    if (was_empty) {
        mark_depth_band(s, tx, ty);
    }
    work->moves++;
    if (work->arrivals != NULL && ty != y) {
        work->arrivals[arrival_byte(s, tx, ty)] |= (uint8_t)(1u << (tx & 7));
    }

    *stayed_in_row = (ty == y);
    if (*stayed_in_row) {
        *touched_x = tx;
    } else {
        mark_move(s, x, y, tx, ty);
    }
    return true;
}

static inline void
union_touched_x(bool* touched, int* x0, int* x1, int lo, int hi) {
    if (!*touched || lo < *x0) {
        *x0 = lo;
    }
    if (!*touched || hi > *x1) {
        *x1 = hi;
    }
    *touched = true;
}

/* One cell's cross-flow contribution: tracked liquid, fold same-row transfer.
 * Split for complexity. */
static inline bool
equalise_one_row_cell(sand_t* s, uint8_t* row, int x, int y, const uint8_t* ax_row, const uint8_t* dg_row,
                      const uint8_t* below_row, int w, const xflow_t* r, int dx, int sight, uint16_t is_liquid,
                      bool* touched, int* touched_x0, int* touched_x1, liquid_work_t* work) {
    const cell_t c = row[x];
    if (CELL_IS_EMPTY(c)) {
        return false;
    }
    const uint8_t id = CELL_MATERIAL(c);
    if (((is_liquid >> id) & 1u) == 0) {
        return false;
    }
    if (work->guard && (work->arrivals[arrival_byte(s, x, y)] & (1u << (x & 7))) != 0) {
        return true;
    }

    /* DERIVED HERE, NOT CARRIED IN, and derived only once the cell is known to
     * be liquid - which ~30% of examined cells are. Walked incrementally by the
     * caller it cost an add, a mask and a spilled load/store for every cell
     * including the ~70% that never reach this line. The phase is a pure
     * function of position: the caller seeded it as (q_q8 * cx_from) & 255 and
     * stepped by +/-q_q8 per cell, which telescopes to (q_q8 * x) & 255 in
     * either direction. Off the major axis it never advanced at all. */
    const int q_q8 = r->q_q8;
    const int pat = (r->ax[0] != 0) ? ((q_q8 * x) & 255) : ((q_q8 * y) & 255);
    const bool diagonal = (pat < q_q8);

    const int px = diagonal ? r->dg[0] : r->ax[0];
    const int py = diagonal ? r->dg[1] : r->ax[1];
    const int bias_q8 = diagonal ? r->bias_dg_q8 : r->bias_ax_q8;
    /* The ray's own row, already resolved once for the whole row walk - the
     * two rays are the only rows a neighbour probe can land in. */
    const uint8_t* const n_row = diagonal ? dg_row : ax_row;

    bool stayed_in_row = false;
    int tx = 0;
    if (equalise_one_cell(s, row, x, y, below_row, n_row, w, px, py, dx, sight, id, CELL_VARIANT(c), bias_q8,
                          &stayed_in_row, &tx, work)
        && stayed_in_row) {
        const int lo = x < tx ? x : tx;
        const int hi = x > tx ? x : tx;

        /* Marking deferred for gravity-free orientations. mark_rows() impact.
         * Narrow x range for wake. */
        union_touched_x(touched, touched_x0, touched_x1, lo, hi);

        /* Woken HERE, per transfer - not once for the whole row's combined
         * span in equalise_one_row(), which unions every transfer's own
         * narrow span first. A settled block with no transfer of its own
         * sat inside that union whenever any OTHER transfer landed on the
         * far side of it, so a wide, mostly-still pool never slept: any one
         * correction anywhere on the row kept the entire span between it
         * and the next one awake. */
        const int by = (int)((unsigned)y / SAND_BLOCK_H);
        wake_blocks_range(s, (int)((unsigned)lo / SAND_BLOCK_W), by, (int)((unsigned)hi / SAND_BLOCK_W), by);
    }
    return true;
}

static inline bool
equalise_one_block(sand_t* s, uint8_t* row, int y, int cx_from, int cx_to, int x_step, const uint8_t* ax_row,
                   const uint8_t* dg_row, const uint8_t* below_row, int w, const xflow_t* r, int dx, int sight,
                   uint16_t is_liquid, bool* touched, int* touched_x0, int* touched_x1, liquid_work_t* work) {
    bool any_liquid = false;

    for (int x = cx_from; x != cx_to; x += x_step) {
        if (equalise_one_row_cell(s, row, x, y, ax_row, dg_row, below_row, w, r, dx, sight, is_liquid, touched,
                                  touched_x0, touched_x1, work)) {
            any_liquid = true;
        }
    }
    return any_liquid;
}

/* A block is SAND_BLOCK_H rows tall, so "liquid is near" holds for every row
 * of a band a pool merely touches, and half of those rows hold nothing at
 * all. Worth its own scan rather than the walk's: one induction variable and
 * an empty test, against the walk's four and a spilled reload per cell. */
static inline bool
span_is_empty(const uint8_t* row, int x0, int x1) {
    for (int x = x0; x < x1; x++) {
        if (!CELL_IS_EMPTY(row[x])) {
            return false;
        }
    }
    return true;
}

/* The answer a skipped span still owes equalise_liquids(): found_any is what
 * clears may_have_liquid. */
static inline bool
span_has_liquid(const uint8_t* row, int x0, int x1, uint16_t is_liquid) {
    for (int x = x0; x < x1; x++) {
        if (((is_liquid >> CELL_MATERIAL(row[x])) & 1u) != 0) {
            return true;
        }
    }
    return false;
}

/* The columns a span's rays can reach: both rays step x by -1, 0 or +1, so the
 * span's own width plus one cell of margin either side, clipped to the grid.
 * A ray leaving the grid sideways is already a reject. */
static inline bool
rays_blocked(const uint8_t* ax_row, const uint8_t* dg_row, int x0, int x1, int w, uint16_t is_liquid) {
    const int sx0 = (x0 > 0) ? x0 - 1 : 0;
    const int sx1 = (x1 < w) ? x1 + 1 : w;

    if (ax_row != NULL && !span_has_no_liquid_room(ax_row, sx0, sx1, is_liquid)) {
        return false;
    }
    /* Equal covers both "one row, two rays" - which is every landscape and
     * diagonal orientation, where ax and dg differ only in x - and "both off
     * the grid". */
    if (dg_row == ax_row) {
        return true;
    }
    return dg_row == NULL || span_has_no_liquid_room(dg_row, sx0, sx1, is_liquid);
}

/* THE AXIS ROW WHERE THE DIAGONAL RAY IS UNREACHABLE: a cell takes that ray
 * only when `pat < q_q8`, and pat is a multiple of q_q8, so a lean of zero
 * tangent reads `0 < 0`. Saying so lets rays_blocked() short-circuit on
 * `dg_row == ax_row` and skip a block on one span. */
static inline const uint8_t*
diagonal_row(const sand_t* s, int y, const xflow_t* r, const uint8_t* ax_row) {
    return (r->q_q8 != 0) ? dest_row(s, y + r->dg[1]) : ax_row;
}

static bool
equalise_one_row(sand_t* s, int y, int w, int x_step, const xflow_t* r, int dx, int dy, int sight, uint16_t is_liquid,
                 liquid_work_t* work) {
    uint8_t* row = s->cells + (size_t)y * (size_t)w;

    const uint8_t* const ax_row = dest_row(s, y + r->ax[1]);
    const uint8_t* const dg_row = diagonal_row(s, y, r, ax_row);
    const uint8_t* const below_row = dest_row(s, y + dy);

    bool any_liquid = false;
    bool touched = false;
    int touched_x0 = 0, touched_x1 = 0;

    const uint8_t* blocks = work->block_read != NULL ? work->block_read : s->block_state;
    const uint8_t* brow = blocks != NULL ? blocks + (size_t)((unsigned)y / SAND_BLOCK_H) * (size_t)s->block_cols : NULL;
    const int bx_from = (x_step > 0) ? 0 : s->block_cols - 1;
    const int bx_to = (x_step > 0) ? s->block_cols : -1;

    for (int bx = bx_from; bx != bx_to; bx += x_step) {
        if (brow != NULL && (brow[bx] & BLOCK_LIQUID_NEAR) == 0) {
            continue;
        }
        const int lo = bx * SAND_BLOCK_W;
        const int hi = (lo + SAND_BLOCK_W < w) ? lo + SAND_BLOCK_W : w;

        /* A block the main sweep left settled had no arrival from gravity OR
         * a prior cross-flow transfer last step - either wakes it directly
         * (wake_block_and_neighbors()/wake_blocks_range()) - so this pass
         * already answered "nothing to find" here, 32 rows' worth of times
         * over one block, and the answer cannot have changed since. Skips
         * past rays_blocked() rediscovering the same thing every row. */
        if (brow != NULL
            && (brow[bx] & (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER))
                   == (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) {
            if (span_has_liquid(row, lo, hi, is_liquid)) {
                any_liquid = true;
            }
            continue;
        }

        if (span_is_empty(row, lo, hi)) {
            continue;
        }

        /* SKIPPED WHOLE when both rays land where nothing can be lower - the
         * shape a SETTLED pool holds, and worth rediscovering every step,
         * because BLOCK_LIQUID_NEAR only says liquid is present, never that
         * it is still moving. Halves a settled basin.
         *
         * RNG-NEUTRAL: these cells would all have rejected at
         * neighbour_is_lower(), and liquid_may_move()'s viscosity roll sits
         * after that, so no draw is skipped. PER BLOCK, NOT PER ROW - a
         * row-level form only fires on a pool spanning the whole screen. */
        if (rays_blocked(ax_row, dg_row, lo, hi, w, is_liquid)) {
            if (span_has_liquid(row, lo, hi, is_liquid)) {
                any_liquid = true;
            }
            continue;
        }

        if (equalise_one_block(s, row, y, (x_step > 0) ? lo : hi - 1, (x_step > 0) ? hi : lo - 1, x_step, ax_row,
                               dg_row, below_row, w, r, dx, sight, is_liquid, &touched, &touched_x0, &touched_x1,
                               work)) {
            any_liquid = true;
        }
    }

    if (touched) {
        s->faller_may_move = true;
        /* The dirty span for drawing is still the whole row's union - a
         * repaint wants everything that changed, unlike the wake above,
         * which equalise_one_row_cell() now does per transfer. */
        mark_row_span(s, y, touched_x0, touched_x1);
    }

    return any_liquid;
}

/* BLOCK_LIQUID_NEAR checks blocks/neighbours with liquid; O(blocks). Expanded
 * for liquid movement. See sand_priv.h for BLOCK_HAS_LIQUID.
 *
 * ORDER-INDEPENDENT over [by_from, by_to), the same way and for the same
 * reason as finalize_settling_range() (sand.c): every iteration only reads
 * BLOCK_HAS_LIQUID, which nothing here writes, and only writes its own
 * block's NEAR bit. */
static void
mark_liquid_neighbourhoods_range(sand_t* s, int by_from, int by_to) {
    for (int by = by_from; by < by_to; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            uint8_t* slot = &s->block_state[by * s->block_cols + bx];
            *slot = block_or_neighbour_has_liquid(s, bx, by) ? (uint8_t)(*slot | BLOCK_LIQUID_NEAR)
                                                             : (uint8_t)(*slot & ~BLOCK_LIQUID_NEAR);
        }
    }
}

typedef struct {
    sand_t* s;
    int by_from, by_to;
} mark_liquid_neighbourhoods_half_t;

_Static_assert(sizeof(mark_liquid_neighbourhoods_half_t) <= JOB_CTX_MAX,
               "mark_liquid_neighbourhoods_half_t must fit JOB_CTX_MAX");

static void
mark_liquid_neighbourhoods_worker(void* ctx) {
    const mark_liquid_neighbourhoods_half_t* half = ctx;
    mark_liquid_neighbourhoods_range(half->s, half->by_from, half->by_to);
}

/* Same threshold as finalize_settling(); below it one core outruns a hop to
 * core 1 and back. */
#define MARK_LIQUID_NEIGHBOURHOODS_SPLIT_MIN_BLOCK_ROWS 4

static void
mark_liquid_neighbourhoods(sand_t* s) {
    if (sand_two_core_step_enabled() && s->block_rows >= MARK_LIQUID_NEIGHBOURHOODS_SPLIT_MIN_BLOCK_ROWS) {
        const int mid = s->block_rows / 2;
        mark_liquid_neighbourhoods_half_t half = {s, mid, s->block_rows};
        (void)job_run_core1(mark_liquid_neighbourhoods_worker, &half, sizeof half);
        mark_liquid_neighbourhoods_range(s, 0, mid);
        (void)job_wait(100);
        return;
    }

    mark_liquid_neighbourhoods_range(s, 0, s->block_rows);
}

typedef struct {
    sand_t local;
    liquid_work_t work;
    uint8_t* blocks;
    uint8_t* dirty;
    uint16_t* x0;
    uint16_t* x1;
    bool found_any;
} liquid_stripe_t;

typedef struct {
    liquid_stripe_t* stripe;
    const xflow_t* flow;
    int dx, dy, sight, offset, color, share;
    uint16_t is_liquid;
} liquid_phase_t;

_Static_assert(sizeof(liquid_phase_t) <= JOB_CTX_MAX, "liquid phase must fit JOB_CTX_MAX");
_Static_assert(LIQUID_STRIPE_H > 2 * SAND_LIQUID_SIGHT, "liquid stripes need an interior beyond both guards");

static void
prepare_liquid_stripe(liquid_stripe_t* stripe, const sand_t* s, uint8_t* arrivals) {
    stripe->local = *s;
    stripe->local.rng_hashed = true;
    stripe->work = (liquid_work_t){.block_read = s->block_state, .arrivals = arrivals};
    stripe->found_any = false;
    if (s->block_state != NULL) {
        stripe->local.block_state = stripe->blocks;
        memcpy(stripe->blocks, s->block_state, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    if (s->dirty_rows != NULL) {
        stripe->local.dirty_rows = stripe->dirty;
        memcpy(stripe->dirty, s->dirty_rows, (size_t)s->h);
    }
    if (s->dirty_x0 != NULL && s->dirty_x1 != NULL) {
        stripe->local.dirty_x0 = stripe->x0;
        stripe->local.dirty_x1 = stripe->x1;
        memcpy(stripe->x0, s->dirty_x0, sizeof *stripe->x0 * (size_t)s->h);
        memcpy(stripe->x1, s->dirty_x1, sizeof *stripe->x1 * (size_t)s->h);
    }
}

static void
merge_liquid_stripe(sand_t* s, const liquid_stripe_t* stripe) {
    if (s->block_state != NULL) {
        for (int i = 0; i < s->block_cols * s->block_rows; i++) {
            const uint8_t local = stripe->blocks[i];
            s->block_state[i] &= (uint8_t)(local | ~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER));
            s->block_state[i] |= local & BLOCK_ACTIVE;
        }
    }
    for (int y = 0; y < s->h; y++) {
        if (s->dirty_rows != NULL) {
            s->dirty_rows[y] |= stripe->dirty[y];
        }
        if (s->dirty_x0 != NULL && s->dirty_x1 != NULL) {
            if (stripe->x0[y] < s->dirty_x0[y]) {
                s->dirty_x0[y] = stripe->x0[y];
            }
            if (stripe->x1[y] > s->dirty_x1[y]) {
                s->dirty_x1[y] = stripe->x1[y];
            }
        }
    }
    s->faller_may_move |= stripe->local.faller_may_move;
    sand_liquid_moves += stripe->work.moves;
    sand_liquid_crossflow_probes += stripe->work.probes;
}

/* Eight guard rows keep every cell access inside its stripe. Sleep decisions
 * use the phase's immutable flags; wake and repaint writes are private until
 * join because their reach exceeds the cell guards. */
static void
liquid_phase_worker(void* arg) {
    const liquid_phase_t* c = arg;
    liquid_stripe_t* stripe = c->stripe;
    sand_t* s = &stripe->local;
    const int y_step = c->flow->dg[1] > 0 ? -1 : 1;
    const int x_step = c->flow->dg[0] > 0 ? -1 : 1;
    int seen = 0;
    for (int k = c->offset == 0 ? 0 : -1; c->offset + k * LIQUID_STRIPE_H < s->h; k++) {
        if (((k % 2) + 2) % 2 != c->color) {
            continue;
        }
        if ((seen++ & 1) != c->share) {
            continue;
        }
        const int band0 = c->offset + k * LIQUID_STRIPE_H;
        const int band1 = band0 + LIQUID_STRIPE_H;
        const int y0 = band0 > 0 ? band0 + c->sight : 0;
        const int y1 = band1 < s->h ? band1 - c->sight : s->h;
        for (int y = y_step > 0 ? y0 : y1 - 1; y >= y0 && y < y1; y += y_step) {
            stripe->found_any |=
                equalise_one_row(s, y, s->w, x_step, c->flow, c->dx, c->dy, c->sight, c->is_liquid, &stripe->work);
        }
    }
}

static bool
liquid_guard_row(int y, int h, int offset, int sight) {
    const int band0 = ((y + LIQUID_STRIPE_H - offset) / LIQUID_STRIPE_H) * LIQUID_STRIPE_H + offset - LIQUID_STRIPE_H;
    const int band1 = band0 + LIQUID_STRIPE_H;
    return (band0 > 0 && y - band0 < sight) || (band1 < h && band1 - y <= sight);
}

/* A received mass must not be forwarded when its guard row runs later.
 * Bytes are padded per row so concurrent stripes never share a bitmap byte,
 * including grids whose width is not a multiple of eight. */
static bool
equalise_liquid_stripes(sand_t* s, const xflow_t* flow, int sight, int dx, int dy, uint16_t is_liquid,
                        bool* found_any) {
    const size_t rows = (size_t)s->h;
    const size_t blocks = (size_t)s->block_cols * (size_t)s->block_rows;
    const size_t arrival_bytes = rows * (((size_t)s->w + 7) / 8);
    liquid_stripe_t* stripes = calloc(1, 2 * sizeof *stripes + 8 * rows + 2 * blocks + 2 * rows + arrival_bytes);
    if (stripes == NULL) {
        return false;
    }
    uint16_t* spans = (uint16_t*)(stripes + 2);
    uint8_t* bytes = (uint8_t*)(spans + 4 * rows);
    for (int i = 0; i < 2; i++) {
        stripes[i].x0 = spans + (size_t)(2 * i) * rows;
        stripes[i].x1 = stripes[i].x0 + rows;
        stripes[i].blocks = bytes + (size_t)i * (blocks + rows);
        stripes[i].dirty = stripes[i].blocks + blocks;
    }
    uint8_t* arrivals = bytes + 2 * (blocks + rows);
    const int offset = sand_stripe_offset(s, LIQUID_STRIPE_H);
    for (int color = 0; color < 2; color++) {
        prepare_liquid_stripe(&stripes[0], s, arrivals);
        prepare_liquid_stripe(&stripes[1], s, arrivals);
        liquid_phase_t ctx = {&stripes[1], flow, dx, dy, sight, offset, color, 1, is_liquid};
        (void)job_run_core1(liquid_phase_worker, &ctx, sizeof ctx);
        ctx.stripe = &stripes[0];
        ctx.share = 0;
        liquid_phase_worker(&ctx);
        (void)job_wait(100);
        for (int i = 0; i < 2; i++) {
            merge_liquid_stripe(s, &stripes[i]);
            *found_any |= stripes[i].found_any;
        }
    }
    liquid_work_t guard = {.arrivals = arrivals, .guard = true};
    const bool was_hashed = s->rng_hashed;
    s->rng_hashed = true;
    const int y_step = flow->dg[1] > 0 ? -1 : 1;
    const int x_step = flow->dg[0] > 0 ? -1 : 1;
    for (int y = y_step > 0 ? 0 : s->h - 1; y >= 0 && y < s->h; y += y_step) {
        if (liquid_guard_row(y, s->h, offset, sight)) {
            *found_any |= equalise_one_row(s, y, s->w, x_step, flow, dx, dy, sight, is_liquid, &guard);
        }
    }
    s->rng_hashed = was_hashed;
    sand_liquid_moves += guard.moves;
    sand_liquid_crossflow_probes += guard.probes;
    free(stripes);
    return true;
}

static void
equalise_liquids(sand_t* s, const xflow_t* f, int sight, int dx, int dy) {
    bool found_any = false;

    if (s->block_state != NULL) {
        mark_liquid_neighbourhoods(s);
    }

    const int px = f->dg[0];
    const int py = f->dg[1];
    const int w = s->w;
    const int h = s->h;

    const uint16_t is_liquid = liquid_mask();

    /* Swept so that whatever is being given to has already been visited. */
    const int y_from = (py > 0) ? h - 1 : 0;
    const int y_to = (py > 0) ? -1 : h;
    const int y_step = (py > 0) ? -1 : 1;

    const int x_step = (px > 0) ? -1 : 1;

    /* See equalise_one_row(). BLOCK_HAS_LIQUID → BLOCK_LIQUID_NEAR. No move
     * cost. */
    if (!sand_two_core_step_enabled() || h < LIQUID_SPLIT_MIN_ROWS
        || !equalise_liquid_stripes(s, f, sight, dx, dy, is_liquid, &found_any)) {
        liquid_work_t work = {0};
        for (int y = y_from; y != y_to; y += y_step) {
            if (equalise_one_row(s, y, w, x_step, f, dx, dy, sight, is_liquid, &work)) {
                found_any = true;
            }
        }
        sand_liquid_moves += work.moves;
        sand_liquid_crossflow_probes += work.probes;
    }

    /* Sound despite the block skipping above: a skipped block has
     * BLOCK_LIQUID_NEAR clear, and sand_priv.h's invariant is that every
     * liquid cell sits in a block whose NEAR bit is set - so a skipped
     * block provably held nothing to find. */
    if (!found_any) {
        s->may_have_liquid = false;
    }
}

/* Every other step. A whole extra traversal measured 22% of the boiler scene;
 * halving how often it runs halved that, and 0.5 rows a step is still a drift
 * - gas rises at about 0.7 and nobody calls that wrong.
 *
 * A block skip buys nothing here: the scenes that regress are liquid-dense,
 * so BLOCK_LIQUID_NEAR is set nearly everywhere in them. */
#define LIQUID_SORT_PERIOD 2

/*
 * Sub-pass: a lighter liquid rises through a denser one.
 *
 * A PASS OF ITS OWN, not the denser cell sinking during the main sweep, whose
 * no-double-move guarantee covers the cell that MOVES and not the one it
 * DISPLACES - so every row's water sank past the same oil in turn, carrying it
 * sixteen rows in a step. Gas has always risen in its own pass.
 */

/* One swap per gravity ray per step: a swap moves two cells, and the displaced
 * heavy cell would otherwise be found again one cell along the ray and pushed
 * through the whole layer. Chunked so any board fits 32 bytes of stack. */
enum { RISE_RAYS = 256 };

static int
liquid_sort_ray_count(const sand_t* s, int dx, int dy) {
    return (dx == 0) ? s->w : (dy == 0) ? s->h : s->w + s->h - 1;
}

static int
liquid_sort_ray(const sand_t* s, int x, int y, int dx, int dy) {
    if (dx == 0) {
        return x;
    }
    if (dy == 0) {
        return y;
    }
    return (dx == dy) ? x - y + s->h - 1 : x + y;
}

typedef struct {
    sand_t* s;
    uint8_t* swapped;
    uint16_t is_liquid;
    int dx;
    int dy;
    int ray_base;
    int x0;
    int xstep;
    int xi_first;
    int ray_step;
    int y0;
    int ystep;
    int yi_first;
} liquid_sort_t;

static liquid_sort_t
liquid_sort_init(sand_t* s, uint8_t* swapped, uint16_t is_liquid, int dx, int dy) {
    const int w = s->w, h = s->h;
    const int x0 = (dx > 0) ? 0 : w - 1, xstep = (dx > 0) ? 1 : -1;

    return (liquid_sort_t){.s = s,
                           .swapped = swapped,
                           .is_liquid = is_liquid,
                           .dx = dx,
                           .dy = dy,
                           .x0 = x0,
                           .xstep = xstep,
                           .xi_first = (dx == 0) ? 0 : 1,
                           .ray_step = (dx == 0)    ? xstep
                                       : (dy == 0)  ? 0
                                       : (dx == dy) ? xstep
                                                    : -xstep,
                           .y0 = (dy > 0) ? 0 : h - 1,
                           .ystep = (dy > 0) ? 1 : -1,
                           .yi_first = (dy == 0) ? 0 : 1};
}

static void
liquid_sort_x_bounds(const liquid_sort_t* sort, int first_ray, int xi_first, int ray_step, int* out_lo, int* out_hi) {
    const int ray_base = sort->ray_base;
    int xi_lo = xi_first, xi_hi = sort->s->w;

    if (ray_step == 0) {
        if (first_ray < ray_base || first_ray - ray_base >= RISE_RAYS) {
            xi_hi = xi_lo;
        }
    } else if (ray_step > 0) {
        const int ray_lo = xi_first + ray_base - first_ray;
        const int ray_hi = xi_first + ray_base + RISE_RAYS - first_ray;
        if (xi_lo < ray_lo) {
            xi_lo = ray_lo;
        }
        if (xi_hi > ray_hi) {
            xi_hi = ray_hi;
        }
    } else {
        const int ray_lo = xi_first + first_ray - ray_base - RISE_RAYS + 1;
        const int ray_hi = xi_first + first_ray - ray_base + 1;
        if (xi_lo < ray_lo) {
            xi_lo = ray_lo;
        }
        if (xi_hi > ray_hi) {
            xi_hi = ray_hi;
        }
    }

    *out_lo = xi_lo;
    *out_hi = xi_hi;
}

static bool
float_liquid_row(const liquid_sort_t* sort, int y) {
    sand_t* const s = sort->s;
    uint8_t* const cells = s->cells;
    uint8_t* const swapped = sort->swapped;
    const int w = s->w;
    const uint16_t is_liquid = sort->is_liquid;
    const int dx = sort->dx, dy = sort->dy;
    const int ray_base = sort->ray_base;
    const bool may_have_viscous_liquid = s->may_have_viscous_liquid;
    const int x0 = sort->x0, xstep = sort->xstep;
    const int xi_first = sort->xi_first, ray_step = sort->ray_step;
    const int uy = y - dy;

    uint8_t* const row = &cells[(size_t)y * (size_t)w];
    uint8_t* const urow = &cells[(size_t)uy * (size_t)w];
    const int first_x = x0 + xi_first * xstep;
    const int first_ray = liquid_sort_ray(s, first_x, y, dx, dy);
    int xi_lo, xi_hi;
    liquid_sort_x_bounds(sort, first_ray, xi_first, ray_step, &xi_lo, &xi_hi);

    bool moved = false;

    for (int xi = xi_lo, ray = first_ray + (xi_lo - xi_first) * ray_step; xi < xi_hi; xi++, ray += ray_step) {
        const int x = x0 + xi * xstep;
        const int ux = x - dx;
        if (((swapped[(ray - ray_base) >> 3] >> ((ray - ray_base) & 7)) & 1u) != 0u) {
            continue;
        }
        const cell_t me = row[x], above = urow[ux];
        if (CELL_IS_EMPTY(me) || CELL_IS_EMPTY(above)) {
            continue;
        }
        const uint8_t mine = CELL_MATERIAL(me), theirs = CELL_MATERIAL(above);
        /* The viscosity roll the ordinary move pays keeps a rise a lazy drift. */
        const bool can_swap =
            ((is_liquid >> mine) & 1u) != 0 && theirs != mine && ((is_liquid >> theirs) & 1u) != 0
            && material_by_id((material_id_t)theirs)->density > material_by_id((material_id_t)mine)->density
            && (!may_have_viscous_liquid || liquid_may_move(s, x, y, mine));
        if (!can_swap) {
            continue;
        }

        urow[ux] = me;
        row[x] = above;
        swapped[(ray - ray_base) >> 3] |= (uint8_t)(1u << ((ray - ray_base) & 7));
        mark_slide(s, x, y, ux, uy);
        wake_block_and_neighbors(s, x, y);
        wake_block_and_neighbors(s, ux, uy);
        moved = true;
    }
    return moved;
}

static bool
float_lighter_liquids(sand_t* s, int dx, int dy) {
    const int h = s->h;
    const uint16_t is_liquid = liquid_mask();
    const int rays = liquid_sort_ray_count(s, dx, dy);
    bool moved = false;
    uint8_t swapped[RISE_RAYS / 8];
    liquid_sort_t sort = liquid_sort_init(s, swapped, is_liquid, dx, dy);

    for (int ray_base = 0; ray_base < rays; ray_base += RISE_RAYS) {
        sort.ray_base = ray_base;
        memset(swapped, 0, sizeof swapped);
        for (int yi = sort.yi_first; yi < h; yi++) {
            const int y = sort.y0 + yi * sort.ystep;
            if (float_liquid_row(&sort, y)) {
                moved = true;
            }
        }
    }
    return moved;
}

void
sand_step_liquids(sand_t* s, const xflow_t* flow, int dx, int dy) {
    if (!s->may_have_liquid) {
        return;
    }

    xflow_t run;
    if (s->liquid_flip) {
        run.ax[0] = flow->ax[0];
        run.ax[1] = flow->ax[1];
        run.dg[0] = flow->dg[0];
        run.dg[1] = flow->dg[1];
        run.bias_ax_q8 = flow->bias_ax_q8;
        run.bias_dg_q8 = flow->bias_dg_q8;
    } else {
        run.ax[0] = -flow->ax[0];
        run.ax[1] = -flow->ax[1];
        run.dg[0] = -flow->dg[0];
        run.dg[1] = -flow->dg[1];
        run.bias_ax_q8 = -flow->bias_ax_q8;
        run.bias_dg_q8 = -flow->bias_dg_q8;
    }
    run.q_q8 = flow->q_q8;

    /* Cross-flow levels both ways. See equalise_liquids(). */
#ifdef DEVICE_BUILD
    const int64_t equalise_t0 = esp_timer_get_time();
#endif
    equalise_liquids(s, &run, SAND_LIQUID_SIGHT, dx, dy);
#ifdef DEVICE_BUILD
    s->pass_us.liquid_us = esp_timer_get_time() - equalise_t0;
#endif
    s->liquid_flip = !s->liquid_flip;

    /* SKIPPED ON ONE BOARD-WIDE FACT. Sorting by density needs two different
     * liquids to sort; with one, or none, every cell of this pass would reject
     * and the answer is the same for all of them. A screen of water - what a
     * liquid scene usually is - therefore pays a popcount, not a pass. */
    const uint16_t liquids_here = s->may_have_materials & liquid_mask();
    if ((liquids_here & (uint16_t)(liquids_here - 1u)) != 0u && (s->step_phase & (LIQUID_SORT_PERIOD - 1u)) == 0u) {
#ifdef DEVICE_BUILD
        const int64_t float_t0 = esp_timer_get_time();
#endif
        (void)float_lighter_liquids(s, dx, dy);
#ifdef DEVICE_BUILD
        s->pass_us.float_us = esp_timer_get_time() - float_t0;
#endif
    }
}
