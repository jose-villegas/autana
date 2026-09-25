/*
 * sand_plants - tree, root and leaf growth: from a bare seed cell to a
 * branching, thickening trunk with a canopy, fed by soil moisture a root
 * system draws down through itself.
 *
 * Shares almost no call graph with sand_reactions.c's fire chemistry -
 * nothing here reads pair_bits[]/PAIR_*, calls try_ignite_given(),
 * conduct_heat() or cool_off_chain(), and nothing over there calls a
 * grow/root/sprout/bud function. Both are reaction_t-driven per-cell
 * passes dispatched by the same step_one_reacting_row()
 * (sand_reactions.c); the handful of helpers genuinely needed by BOTH
 * halves (place_cell()/place_reacted(), reaction_dirs, pay_quench_cost(),
 * soil_set_moisture()) live in sand_priv.h instead, the same way sand.c
 * and sand_liquid.c already share what a gravity-ward move and a
 * cross-flow pass both need.
 *
 * A stem walk (stem_next()/find_water()) carries every grower's question -
 * is there water, is there room - down through however much of the plant
 * sits between a leaf and the ground. step_one_growing_cell() is the one
 * that actually shapes a tree: height, lean, branch or thicken, then
 * harden a mature run into wood with a canopy on top.
 */

#include "reaction_doc.h"
#include "sand_priv.h"

/* FALLS in cold pass, not sweep. Gravity-ward, checks space. Determines (ax,
 * ay) type. */
static inline bool
is_kin(cell_t a, cell_t self, const reaction_t* r) {
    return a == self || (r->clings_to != 0 && CELL_MATERIAL(a) == r->clings_to);
}

/* Leaf-to-roots mimic, efficient on crowded boards. Larger bodies shed outer
 * cells. */
#define SUPPORT_MAX   48

/* Membership index over body[], which stays a queue because the walk order is
 * its job. A power of two over SUPPORT_MAX leaves a full body a quarter of the
 * slots free.
 *
 * Asking body[] itself was quadratic: 829,000 comparisons a step on a poured
 * heap. A scene with plants but nothing falling cannot see it. */
#define SUPPORT_SLOTS 64

static inline unsigned
support_slot(uint16_t at) {
    return (((unsigned)at * 2654435761u) >> 26) & (SUPPORT_SLOTS - 1u);
}

typedef struct {
    uint16_t body[SUPPORT_MAX];
    uint16_t slot[SUPPORT_SLOTS];
    uint64_t filled;
    int n;
} support_t;

static void
support_admit(support_t* sp, uint16_t at) {
    unsigned k = support_slot(at);
    while (((sp->filled >> k) & 1u) != 0u) {
        if (sp->slot[k] == at) {
            return;
        }
        k = (k + 1u) & (SUPPORT_SLOTS - 1u);
    }
    sp->slot[k] = at;
    sp->filled |= (uint64_t)1u << k;
    sp->body[sp->n++] = at;
}

/* True once a neighbour of body cell `at` that is not kin sits straight
 * down: the body rests on it. Kin neighbours join the body instead. */
static bool
support_visit(sand_t* s, support_t* sp, int at, int w, int h, int down, cell_t self, const reaction_t* r) {
    const int cx = at % w, cy = at / w;

    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(down + d);
        const int nx = cx + nd[0], ny = cy + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t c = s->cells[nat];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        if (!is_kin(c, self, r)) {
            /* Diagonals not counted; wall sticks in narrow shafts. */
            if (d == 0) {
                return true; /* this body is resting on something */
            }
            continue;
        }
        if (sp->n >= SUPPORT_MAX) {
            continue; /* too big to finish; treat as loose */
        }
        support_admit(sp, (uint16_t)nat);
    }
    return false;
}

static bool
anchored(sand_t* s, int x, int y, int w, int h, cell_t self, const reaction_t* r) {
    support_t sp;
    sp.filled = 0;
    sp.n = 0;
    int head = 0;

    const uint16_t seed = (uint16_t)((size_t)y * (size_t)w + (size_t)x);
    sp.slot[support_slot(seed)] = seed;
    sp.filled |= (uint64_t)1u << support_slot(seed);
    sp.body[sp.n++] = seed;

    const int down = ring_of(s->last_load_dx, s->last_load_dy);

    while (head < sp.n) {
        if (support_visit(s, &sp, (int)sp.body[head++], w, h, down, self, r)) {
            return true;
        }
    }
    return false;
}

bool
faller_can_move(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const int nx = x + s->last_load_dx;
    const int ny = y + s->last_load_dy;
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    if (!CELL_IS_EMPTY(s->cells[nat])) {
        return false; /* landed */
    }
    return !anchored(s, x, y, w, h, s->cells[at], r);
}

bool
step_one_falling_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    if (!faller_can_move(s, x, y, w, h, r)) {
        return false;
    }
    const int nx = x + s->last_load_dx;
    const int ny = y + s->last_load_dy;
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->falls) {
        return true; /* still falling, just not now */
    }

    s->cells[nat] = s->cells[at];
    s->cells[at] = SAND_EMPTY;
    mark_move(s, x, y, nx, ny);
    return true;
}

/* Growth stops when soil dries. Bound to prevent unnecessary searches. */
#define GROW_REACH  48

/* Three for crown, small enough for shaping. */
#define CANOPY_SPAN 3

/* See step_one_budding_cell() - water bounds bud. */
#define BUD_COST    3

/* Root depth balance: avoids board starvation, ensures survival. */
#define ROOT_REACH  6

/* Plant stems restrict water flow, capping tree height and branch spread,
 * guided by water paths or moisture availability. */
#define TREE_LIFT   10

/* Thickening turns sapling into trunk; cells nearest ground have unlimited
 * lift. */
#define TRUNK_WIDTH 3

/* One step of the stem walk: where it goes next, and whether that cell is
 * ground (the walk ends there) or a root (it counts toward root_depth). */
typedef struct {
    int x, y;
    bool on_soil;
    bool via_root;
} stem_step_t;

/* The one-cell root lookahead runs before the ordinary stem/ground scan, and
 * must: reversing the order changes which cell a root-backed stem commits
 * to. Looking one cell ahead, rather than only at the current cell, keeps the
 * walk from losing the bed when it shifts by one row. */
static bool
root_lookahead(sand_t* s, int cx, int cy, int w, int h, const reaction_t* r, int down, stem_step_t* st) {
    const int* fd = ring_dir(down);
    const int tx = cx + fd[0], ty = cy + fd[1];
    if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h
        || s->cells[(size_t)ty * (size_t)w + (size_t)tx] != (cell_t)r->roots_to) {
        return false;
    }
    const int ax = tx + s->last_load_dx, ay = ty + s->last_load_dy;
    if ((unsigned)ax >= (unsigned)w || (unsigned)ay >= (unsigned)h) {
        return false;
    }
    const cell_t under = s->cells[(size_t)ay * (size_t)w + (size_t)ax];
    if (CELL_IS_EMPTY(under) || (reaction_of(under)->soil == 0 && under != (cell_t)r->roots_to)) {
        return false;
    }
    st->x = tx;
    st->y = ty;
    st->via_root = true;
    return true;
}

/* Straight down, then one step round either way. */
static const int below_fan[3] = {0, 1, 7};

static void
scan_below(sand_t* s, int cx, int cy, int w, int h, const reaction_t* r, cell_t self, int down, stem_step_t* st) {
    for (int i = 0; i < 3; i++) {
        const int* fd = ring_dir(down + below_fan[i]);
        const int tx = cx + fd[0], ty = cy + fd[1];
        if ((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) {
            continue;
        }
        const cell_t c = s->cells[(size_t)ty * (size_t)w + (size_t)tx];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        if (reaction_of(c)->soil != 0) {
            st->x = tx;
            st->y = ty;
            st->on_soil = true;
            return; /* ground: stop looking for stem */
        }
        if (st->x >= 0) {
            continue;
        }
        if (is_kin(c, self, r)) {
            st->x = tx;
            st->y = ty; /* more stem, keep it as a fallback */
        } else if (r->roots_to != 0 && c == (cell_t)r->roots_to) {
            /* ROOT counts as stem, or a root-backed stem would find
             * neither stem nor ground. */
            st->x = tx;
            st->y = ty;
            st->via_root = true;
        }
    }
}

/* Growth seeks nutrient-rich soil, drinking seeks room to expand. A lit cell
 * never has room or water. */
static bool
soil_offers(cell_t c, bool wants_room) {
    const reaction_t* cr = reaction_of(c);
    if (cell_is_burning(c)) {
        return false;
    }
    return wants_room ? moisture_of(c, cr) < cr->moist_max : moisture_of(c, cr) != 0;
}

/* From the collar at (cx, cy), gravity-ward through at most ROOT_REACH cells
 * of soil to the first one that offers what the caller wants. */
static int
drink_below_collar(sand_t* s, int cx, int cy, int w, int h, const reaction_t* r, bool wants_room) {
    const int dx = s->last_load_dx, dy = s->last_load_dy;
    for (int depth = 0; depth < ROOT_REACH; depth++) {
        if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
            return -1;
        }
        const size_t at = (size_t)cy * (size_t)w + (size_t)cx;
        const cell_t c = s->cells[at];
        /* Root must be TRANSPARENT to avoid cutting off water. */
        if (r->roots_to != 0 && c == (cell_t)r->roots_to) {
            cx += dx;
            cy += dy;
            continue;
        }
        if (CELL_IS_EMPTY(c) || reaction_of(c)->soil == 0) {
            return -1;
        }
        if (soil_offers(c, wants_room)) {
            return (int)at;
        }
        cx += dx;
        cy += dy;
    }
    return -1;
}

/* Soil soaks bottom-up and dries top-down, so the collar can be dry over
 * wet rows: past the stem, the walk goes up to ROOT_REACH cells into the
 * soil for water. Roots on the stem count toward root_depth, not lift. */
static int
find_water(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self, int* lift, int* contact_at,
           int* root_depth, bool wants_room) {
    const int down = ring_of(s->last_load_dx, s->last_load_dy);

    *contact_at = -1;

    int cx = x, cy = y;
    int lift_count = 0;
    int roots_passed = 0;
    for (int step = 0; step < GROW_REACH; step++) {
        /* `lift_count` tracks STEM transitions; `step` reuses fails, counting
         * roots incorrectly. */
        *lift = lift_count;
        *root_depth = roots_passed;
        stem_step_t st = {-1, -1, false, false};

        /* COMMITTED to root skips the scan. Not `st.x >= 0`: testing that
         * would stop the scan on a fallback and miss ground. */
        if (r->roots_to == 0 || !root_lookahead(s, cx, cy, w, h, r, down, &st)) {
            scan_below(s, cx, cy, w, h, r, self, down, &st);
        }
        if (st.x < 0) {
            return -1; /* neither stem nor ground below */
        }
        cx = st.x;
        cy = st.y;
        if (st.on_soil) {
            /* Into the soil. This is the collar. */
            *contact_at = (int)((size_t)cy * (size_t)w + (size_t)cx);
            return drink_below_collar(s, cx, cy, w, h, r, wants_room);
        }
        if (st.via_root) {
            roots_passed++;
        } else {
            lift_count++;
        }
    }
    return -1;
}

/* The GROWER half of reaction_t.roots (material.h): fires once, at
 * root_depth == 0, welding CONTACT into the FIRST root; step_one_rooting_cell()
 * below is the ROOT half, growth from a root already placed, so the two are
 * gated apart. Moisture is spent at soil_at, which can sit cells away from
 * contact_at - only contact_at ever converts, so a deep drink never leaves a
 * disconnected root speck. */
static void
spend_soil_moisture(sand_t* s, int w, const reaction_t* r, int soil_at, uint8_t amount, int contact_at,
                    int root_depth) {
    const cell_t soil = s->cells[soil_at];
    s->cells[soil_at] = soil_set_moisture(soil, (uint8_t)(moisture_of(soil, reaction_of(soil)) - amount), 0);
    mark_rows(s, soil_at % w, soil_at / w, soil_at / w);

    if (r->roots == 0 || contact_at < 0 || root_depth != 0) {
        return;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return;
    }
    place_reacted(s, contact_at % w, contact_at / w, (size_t)contact_at, r->roots_to);
}

/* A root with more root neighbours than this stops growing, keeping roots
 * filaments, not slabs. */
#define ROOT_SURFACE_MAX    2

#define ROOT_WEIGHT_AWAY    2
#define ROOT_WEIGHT_DOWN    2

#define ROOT_CONDUCT_CHANCE 64

/* A soil cell a root conducts between: its index, position and moisture. */
typedef struct {
    int at, x, y, m;
} conduit_end_t;

static void
conduit_take(conduit_end_t* e, int m, size_t nat, int nx, int ny) {
    e->m = m;
    e->at = (int)nat;
    e->x = nx;
    e->y = ny;
}

/* Weighs neighbour k (round from straight down) as the driest sink below or
 * the wettest source beside or above. */
static void
conduit_weigh(cell_t c, int k, size_t nat, int nx, int ny, conduit_end_t* src, conduit_end_t* dst) {
    if (CELL_IS_EMPTY(c)) {
        return;
    }
    const reaction_t* cr = reaction_of(c);
    if (cr->soil == 0) {
        return; /* not soil: root, wood, stone, air, gunpowder - a fuse
                 * is not ground a root conducts water through. */
    }
    const int m = moisture_of(c, cr);
    if (k == 0 || k == 1 || k == 7) {
        /* Only a cell below moist_max can receive more. moisture_of()
         * reads a burning cell as 0 regardless of its real moisture -
         * !cell_is_burning(c) keeps that from reading as the thirstiest
         * candidate. */
        if (m < cr->moist_max && !cell_is_burning(c) && (dst->at < 0 || m < dst->m)) {
            conduit_take(dst, m, nat, nx, ny);
        }
    } else if (m > src->m) {
        /* Beside or above: a source, if it holds anything. */
        conduit_take(src, m, nat, nx, ny);
    }
}

/* One ROOT cell, carrying water down through itself as a conduit - moves
 * gravity-ward only, but its side and upper neighbours all count as
 * sources, not just the one directly above, so a whole column of soil can
 * drain through it rather than only the cell it sits under. */
bool
step_one_conducting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const int down = ring_of(s->last_load_dx, s->last_load_dy);
    (void)r;

    conduit_end_t src = {-1, 0, 0, 0};
    conduit_end_t dst = {-1, 0, 0, 0};

    for (int k = 0; k < 8; k++) {
        const int* nd = ring_dir(down + k);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        conduit_weigh(s->cells[nat], k, nat, nx, ny, &src, &dst);
    }
    if (src.at < 0 || dst.at < 0) {
        return false; /* nothing to carry, or nowhere to carry it */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= ROOT_CONDUCT_CHANCE) {
        return true;
    }
    const cell_t src_c = s->cells[src.at], dst_c = s->cells[dst.at];
    s->cells[src.at] = soil_set_moisture(src_c, (uint8_t)(src.m - 1), (uint8_t)(dst.m + 1));
    s->cells[dst.at] = with_moisture(dst_c, (uint8_t)(dst.m + 1), reaction_of(dst_c));
    mark_rows(s, src.x, src.y, src.y);
    mark_rows(s, dst.x, dst.y, dst.y);
    wake_block_and_neighbors(s, src.x, src.y);
    wake_block_and_neighbors(s, dst.x, dst.y);
    return true;
}

static int
count_root_neighbors(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    int root_neighbors = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        if (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == (cell_t)r->roots_to) {
            root_neighbors++;
        }
    }
    return root_neighbors;
}

/* Sum of the directions pointing away from every root or wood neighbour. */
static void
away_from_parent(sand_t* s, int x, int y, int w, int h, const reaction_t* r, int* away_x, int* away_y) {
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t c = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (c == (cell_t)r->roots_to || (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == r->clings_to)) {
            *away_x -= nd[0];
            *away_y -= nd[1];
        }
    }
}

/* Moist soil cells a root could grow into, each with its pick weight. */
typedef struct {
    int at[8], x[8], y[8], w[8];
    int n, total_w;
} root_cands_t;

static int
root_weight(const int* nd, int away_x, int away_y, int gx, int gy) {
    int wgt = 1;
    if (nd[0] * away_x + nd[1] * away_y > 0) {
        wgt += ROOT_WEIGHT_AWAY; /* carries on away from its parent */
    }
    if (nd[0] * gx + nd[1] * gy > 0) {
        wgt += ROOT_WEIGHT_DOWN; /* reaches down */
    }
    return wgt;
}

static void
gather_root_cands(sand_t* s, int x, int y, int w, int h, int away_x, int away_y, root_cands_t* rc) {
    const int gx = s->last_load_dx, gy = s->last_load_dy;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n) || reaction_of(n)->soil == 0 || moisture_of(n, reaction_of(n)) == 0) {
            continue;
        }
        const int wgt = root_weight(nd, away_x, away_y, gx, gy);
        rc->at[rc->n] = (int)nat;
        rc->x[rc->n] = nx;
        rc->y[rc->n] = ny;
        rc->w[rc->n] = wgt;
        rc->total_w += wgt;
        rc->n++;
    }
}

/* A ROOT cell rolls to convert one adjacent moist soil cell into more root -
 * see docs/sand/Sand-Simulation.md and ROOT_SURFACE_MAX above for why
 * root_neighbors caps it before any roll happens. */
bool
step_one_rooting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    /* Cheapest question first, rejects thick columns without neighbour scan
     * touching RNG. */
    if (count_root_neighbors(s, x, y, w, h, r) > ROOT_SURFACE_MAX) {
        return false; /* buried inside its own kind; nothing to do here */
    }

    /* The pick below is WEIGHTED, not uniform: a direction that continues
     * away from the parent (wood, or an existing root) or that reaches
     * downward carries more weight, so a root spreads sideways from where
     * it started and seeks water beneath itself rather than beside it -
     * with no root or wood beside it, gravity alone steers. */
    int away_x = 0, away_y = 0;
    away_from_parent(s, x, y, w, h, r, &away_x, &away_y);

    root_cands_t rc;
    rc.n = 0;
    rc.total_w = 0;
    gather_root_cands(s, x, y, w, h, away_x, away_y, &rc);
    if (rc.n == 0) {
        return false; /* nothing moist beside it right now */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->roots) {
        return true; /* a candidate exists; just not this roll */
    }
    int pick = rng_below(&s->rng, rc.total_w);
    int k = 0;
    while (pick >= rc.w[k]) {
        pick -= rc.w[k];
        k++;
    }
    place_reacted(s, rc.x[k], rc.y[k], (size_t)rc.at[k], r->roots_to);
    return true;
}

/* DRINKS adds moisture. `KIND_STATIC` blocks water via foliage. Water moves
 * from stem to roots. */
bool
step_one_drinking_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r, cell_t self) {
    int lx = -1, ly = -1;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_LIQUID && reaction_of(n)->wets != 0) {
            lx = nx;
            ly = ny;
            break;
        }
    }
    if (lx < 0) {
        return false; /* nothing to drink */
    }

    int lift = 0, contact_at = -1, root_depth = 0; /* drinking never spends
                                                     * soil moisture, so
                                                     * nothing here roots -
                                                     * scratch values */
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, true);
    /* FALSE, though a drink is possible: the caller reads this as "soil
     * moisture was made", and a plant standing in water with no soil under it
     * makes none. Saying true there kept the growth stages armed off a puddle
     * nothing could reach. Costs no drinking - this stage is gated on liquid,
     * not on moisture. */
    if (soil_at < 0) {
        return false;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->drinks) {
        return false;
    }

    pay_quench_cost(s, lx, ly, w);

    const cell_t soil = s->cells[soil_at];
    const reaction_t* sr = reaction_of(soil);
    s->cells[soil_at] = with_moisture(soil, (uint8_t)(moisture_of(soil, sr) + 1), sr);
    mark_rows(s, soil_at % w, soil_at / w, soil_at / w);
    wake_block_and_neighbors(s, soil_at % w, soil_at / w);
    return true;
}

/* SPROUTS: Consume soil, close loop, grow into wood, end trees, leave posts.
 * Trunks live. */
bool
step_one_sprouting_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    int soil_at = -1, empty_at = -1, ex = 0, ey = 0;

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            if (empty_at < 0) {
                empty_at = (int)nat;
                ex = nx;
                ey = ny;
            }
            continue;
        }
        if (soil_at < 0 && reaction_of(n)->soil != 0 && moisture_of(n, reaction_of(n)) != 0) {
            soil_at = (int)nat;
        }
    }
    if (soil_at < 0 || empty_at < 0) {
        return soil_at >= 0;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->sprouts) {
        return true;
    }

    place_reacted(s, ex, ey, (size_t)empty_at, r->sprouts_to);

    /* SPENDS, NEVER SEEDS -1. 0 reports collar, disconnected roots. Sprouting
     * pays for leaf. */
    spend_soil_moisture(s, w, r, soil_at, 1, -1, 0);
    return true;
}

static bool
is_crowned(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    bool crowned = false;
    for (int d = 0; d < 8 && !crowned; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        crowned = (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == (cell_t)r->sprouts_to);
    }
    return crowned;
}

/* Somewhere to put a bud, up and away from gravity; -1 when boxed in. */
static int
find_bud_site(sand_t* s, int x, int y, int w, int h, int* bx, int* by) {
    const int up_i = ring_of(-s->last_load_dx, -s->last_load_dy);
    static const int out[5] = {7, 0, 1, 2, 6};
    for (int d = 0; d < 5; d++) {
        const int* nd = ring_dir(up_i + out[d]);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        if (CELL_IS_EMPTY(s->cells[nat])) {
            *bx = nx;
            *by = ny;
            return (int)nat;
        }
    }
    return -1;
}

bool
step_one_budding_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    if (!is_crowned(s, x, y, w, h, r)) {
        return false;
    }
    const int ax = x - s->last_load_dx, ay = y - s->last_load_dy;
    if ((unsigned)ax < (unsigned)w && (unsigned)ay < (unsigned)h
        && s->cells[(size_t)ay * (size_t)w + (size_t)ax] == self) {
        return false;
    }

    int bx = 0, by = 0;
    const int at = find_bud_site(s, x, y, w, h, &bx, &by);
    if (at < 0) {
        return true; /* crowned, but boxed in */
    }

    int lift = 0, contact_at = -1, root_depth = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false);
    if (soil_at < 0) {
        return true; /* nothing to drink */
    }
    const cell_t soil = s->cells[soil_at];
    if (moisture_of(soil, reaction_of(soil)) < BUD_COST) {
        return true;
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->buds) {
        return true;
    }

    place_reacted(s, bx, by, (size_t)at, r->buds_to);

    spend_soil_moisture(s, w, r, soil_at, BUD_COST, contact_at, root_depth);
    return true;
}

/* Stems grow along DITHERED gravity, alternating steps. Prevents rigid
 * angles, makes trunk wander. */
static bool
stem_next(sand_t* s, int x, int y, int ux, int uy, int w, int h, cell_t self, int* ox, int* oy) {
    const int up = ring_of(ux, uy);
    for (int i = 0; i < 3; i++) {
        const int* d = ring_dir(up + (i == 0 ? 0 : i == 1 ? 1 : 7));
        const int nx = x + d[0], ny = y + d[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        if (s->cells[(size_t)ny * (size_t)w + (size_t)nx] == self) {
            *ox = nx;
            *oy = ny;
            return true;
        }
    }
    return false;
}

/* Bound so plant under half board does not walk it */
#define PUSH_REACH 8

/* Shift cells (dx, dy) from (gx, gy), halting at STATICs. Checks if (gx, gy)
 * is free. */
static bool
shove_aside(sand_t* s, int gx, int gy, int dx, int dy, int w, int h) {
    int ex = gx, ey = gy;
    int run = 0;

    while (run < PUSH_REACH) {
        if ((unsigned)ex >= (unsigned)w || (unsigned)ey >= (unsigned)h) {
            return false; /* shoved into the wall */
        }
        const cell_t c = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        if (CELL_IS_EMPTY(c)) {
            break; /* somewhere to put it all */
        }
        if (material_of(c)->kind == KIND_STATIC) {
            return false; /* will not budge */
        }
        ex += dx;
        ey += dy;
        run++;
    }
    if (run == 0) {
        return true; /* was empty to begin with */
    }
    if (run >= PUSH_REACH) {
        return false; /* too much of it to lift */
    }

    /* Back to front, so nothing is overwritten before it has moved. */
    for (int i = 0; i < run; i++) {
        const int tx = ex, ty = ey;
        ex -= dx;
        ey -= dy;
        s->cells[(size_t)ty * (size_t)w + (size_t)tx] = s->cells[(size_t)ey * (size_t)w + (size_t)ex];
        mark_rows(s, tx, ty, ty);
        wake_block_and_neighbors(s, tx, ty);
    }
    s->cells[(size_t)gy * (size_t)w + (size_t)gx] = SAND_EMPTY;
    mark_rows(s, gx, gy, gy);
    wake_block_and_neighbors(s, gx, gy);
    return true;
}

/* Walks up to `steps` stem cells from (*px, *py) toward (ux, uy), leaving
 * (*px, *py) on the last one reached; returns how many steps it took. */
static int
stem_walk(sand_t* s, int* px, int* py, int ux, int uy, int w, int h, cell_t self, int steps) {
    int taken = 0;
    while (taken < steps) {
        int nx, ny;
        if (!stem_next(s, *px, *py, ux, uy, w, h, self, &nx, &ny)) {
            break;
        }
        *px = nx;
        *py = ny;
        taken++;
    }
    return taken;
}

/* Reach aids, BURIED stagnate, growth peaks early, surface optimal, dense
 * skip scans. The board edge counts as crowd. */
static int
count_packed(sand_t* s, int x, int y, int w, int h, cell_t self, const reaction_t* r) {
    int packed = 0;
    for (int d = 0; d < 8; d++) {
        const int* nd = ring_dir(d);
        const int nx = x + nd[0], ny = y + nd[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            packed++;
            continue;
        }
        if (is_kin(s->cells[(size_t)ny * (size_t)w + (size_t)nx], self, r)) {
            packed++;
        }
    }
    return packed;
}

/* Where along a stem of `run` cells to grow, and which way: `site` counts
 * stem cells up from the grower, `turn` is steps round the ring from the
 * limb's heading. */
typedef struct {
    int site;
    int turn;
    bool thicken;
} grow_plan_t;

static grow_plan_t
plan_growth(sand_t* s, int run, int side) {
    grow_plan_t p = {run - 1, 0, false};
    const int what = rng_below(&s->rng, 8);
    if (what < 4 || run < 3) {
        return p; /* HEIGHT: straight on from the tip */
    }
    if (what < 6) {
        p.turn = side; /* LEAN: the tip, one step round */
        return p;
    }
    if (what < 7) {
        p.site = rng_below(&s->rng, run - 1); /* BRANCH: out and up */
        p.turn = side;
        return p;
    }
    /* WIDTH simulates tree growth by thickening trunk. */
    p.site = rng_below(&s->rng, (run + 1) / 2);
    p.turn = side * 2; /* square on to the run */
    p.thicken = true;
    return p;
}

/* On a won roll, keep the limb's own existing direction (from the previous
 * segment to this one) instead of snapping back toward straight up. */
static int
grow_heading(sand_t* s, int sx, int sy, int ux, int uy, int w, int h, cell_t self, const reaction_t* r) {
    int head = ring_of(ux, uy);
    if (r->holds_line != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->holds_line) {
        /* One stem cell back is the whole baseline: a longer one measured
         * more horizontal drift (38 against 24). */
        int px, py;
        if (stem_next(s, sx, sy, -ux, -uy, w, h, self, &px, &py)) {
            head = ring_of(sx - px, sy - py);
        }
    }
    return head;
}

/* TAPERED: allowance shrinks with height, fat at foot, single cell by
 * branches. Uniform grows a pillar, not a tree. True when the trunk at
 * (sx, sy) should not widen any further along (dx, dy). */
static bool
thick_enough(sand_t* s, int sx, int sy, int dx, int dy, int w, int h, cell_t self, const reaction_t* r, int height) {
    const int allowed = TRUNK_WIDTH - height / 3;
    if (allowed < 2) {
        return true; /* too high up to be thickening */
    }
    int wide = 0;
    for (int i = 1; i < allowed; i++) {
        const int wx = sx + dx * i;
        const int wy = sy + dy * i;
        if ((unsigned)wx >= (unsigned)w || (unsigned)wy >= (unsigned)h) {
            break;
        }
        if (!is_kin(s->cells[(size_t)wy * (size_t)w + (size_t)wx], self, r)) {
            break;
        }
        wide++;
    }
    return wide >= allowed - 1;
}

/* Only a shoot (growth at the tip) may shove what is in its way aside. */
static bool
grow_into(sand_t* s, int gx, int gy, int dx, int dy, int w, int h, cell_t self, bool shoot) {
    if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
        return false;
    }
    const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
    if (!CELL_IS_EMPTY(s->cells[gat]) && !(shoot && shove_aside(s, gx, gy, dx, dy, w, h))) {
        return false; /* in the way, and will not move */
    }
    s->cells[gat] = self;
    latch_content_flags(s, self);
    mark_rows(s, gx, gy, gy);
    wake_block_and_neighbors(s, gx, gy);
    return true;
}

/* Taper linear by LENGTH. Step is taper. Twelve hardenings merge trees; 6-7
 * preferred. */
static void
widen_wood(sand_t* s, int cx, int cy, int up_i, int extra, int w, int h, const reaction_t* r) {
    for (int g = 1; g <= extra; g++) {
        const int sidei = (g & 1) ? 2 : 6; /* square on, both ways */
        const int* gd = ring_dir(up_i + sidei);
        const int gx = cx + gd[0] * ((g + 1) / 2);
        const int gy = cy + gd[1] * ((g + 1) / 2);
        if ((unsigned)gx >= (unsigned)w || (unsigned)gy >= (unsigned)h) {
            continue;
        }
        const size_t gat = (size_t)gy * (size_t)w + (size_t)gx;
        if (!CELL_IS_EMPTY(s->cells[gat])) {
            continue;
        }
        place_cell(s, gx, gy, gat, CELL_MAKE(r->hardens_to, 0));
    }
}

/* The top CANOPY_SPAN cells of the hardened run, lowest first. */
typedef struct {
    int x[CANOPY_SPAN], y[CANOPY_SPAN];
    int n;
} crown_t;

static void
crown_push(crown_t* t, int cx, int cy) {
    if (t->n < CANOPY_SPAN) {
        t->x[t->n] = cx;
        t->y[t->n] = cy;
        t->n++;
        return;
    }
    for (int k = 1; k < CANOPY_SPAN; k++) {
        t->x[k - 1] = t->x[k];
        t->y[k - 1] = t->y[k];
    }
    t->x[CANOPY_SPAN - 1] = cx;
    t->y[CANOPY_SPAN - 1] = cy;
}

static void
hang_canopy(sand_t* s, const crown_t* t, int up_i, int w, int h, const reaction_t* r) {
    static const int crown[4] = {7, 1, 2, 6};
    for (int i = 0; i < t->n; i++) {
        for (int c = 0; c < 4; c++) {
            const int* cd = ring_dir(up_i + crown[c]);
            const int lx = t->x[i] + cd[0], ly = t->y[i] + cd[1];
            if ((unsigned)lx >= (unsigned)w || (unsigned)ly >= (unsigned)h) {
                continue;
            }
            const size_t lat = (size_t)ly * (size_t)w + (size_t)lx;
            if (!CELL_IS_EMPTY(s->cells[lat])) {
                continue;
            }
            if ((int)(rng_next(&s->rng) & 0xFF) >= r->canopy) {
                continue;
            }
            place_reacted(s, lx, ly, lat, r->canopy_to);
        }
    }
}

/* SHAPING PASS: Hardens the `hard` cells from the foot (fx, fy), converts
 * to wood, thickens trunk, adds canopy. Last cell green. Growth ends. */
static void
shape_tree(sand_t* s, int fx, int fy, int ux, int uy, int w, int h, cell_t self, const reaction_t* r, int hard) {
    const int up_i = ring_of(ux, uy);

    crown_t top;
    top.n = 0;

    int cx = fx, cy = fy;
    for (int i = 0; i < hard; i++) {
        int nx = 0, ny = 0;
        const bool more = stem_next(s, cx, cy, ux, uy, w, h, self, &nx, &ny);
        place_cell(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx, CELL_MAKE(r->hardens_to, 0));

        const int span = (hard > 1) ? hard - 1 : 1;
        widen_wood(s, cx, cy, up_i, (int)r->trunk_girth * (span - i) / span, w, h, r);

        /* Hang crown once cells below are wood, avoid foliage. stem_next()
         * still walking. */
        crown_push(&top, cx, cy);

        if (!more) {
            break;
        }
        cx = nx;
        cy = ny;
    }

    if (r->canopy != 0 && r->canopy_to != 0) {
        hang_canopy(s, &top, up_i, w, h, r);
    }
}

/* HARDENING. Counted from the bottom; run measured once regardless of
 * growth. */
static void
harden_stem(sand_t* s, int x, int y, int ux, int uy, int w, int h, cell_t self, const reaction_t* r) {
    int cx = x, cy = y;
    stem_walk(s, &cx, &cy, -ux, -uy, w, h, self, GROW_REACH);
    const int fx = cx, fy = cy;

    const int trunk = 1 + stem_walk(s, &cx, &cy, ux, uy, w, h, self, GROW_REACH - 1);
    if (trunk < r->harden_run) {
        return;
    }

    /* Runs harden before long, wood does not grow. */
    if (__builtin_expect((int)(rng_next(&s->rng) & 0xFF) >= r->harden_chance, 1)) {
        return;
    }
    /* Growth from crowned wood (reaction_t.buds). */
    shape_tree(s, fx, fy, ux, uy, w, h, self, r, trunk);
}

bool
step_one_growing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t self = s->cells[(size_t)y * (size_t)w + (size_t)x];

    /* Prevent rigid stems using dithered sweep. */
    const int ux = -s->last_step_dx;
    const int uy = -s->last_step_dy;
    if (ux == 0 && uy == 0) {
        return true; /* free fall: no up to grow towards */
    }

    if (count_packed(s, x, y, w, h, self, r) >= 5) {
        return true; /* inside the crowd, not at its edge */
    }

    int lift = 0, contact_at = -1, root_depth = 0;
    const int soil_at = find_water(s, x, y, w, h, r, self, &lift, &contact_at, &root_depth, false);
    if (soil_at < 0) {
        return true; /* nothing to drink */
    }
    if (lift >= TREE_LIFT) {
        return true; /* too high up to be fed */
    }
    if ((int)(rng_next(&s->rng) & 0xFF) >= r->grows) {
        return true;
    }

    int tx = x, ty = y;
    const int run = 1 + stem_walk(s, &tx, &ty, ux, uy, w, h, self, GROW_REACH);

    /* Round RING, not up-plus-perpendicular: one step round is adjacent. */
    const int side = rng_below(&s->rng, 2) ? 1 : 7; /* +1 or -1 round */
    const grow_plan_t plan = plan_growth(s, run, side);

    /* Back up the stem to the chosen site, the same way. */
    int sx = x, sy = y;
    stem_walk(s, &sx, &sy, ux, uy, w, h, self, plan.site);

    const int* hd = ring_dir(grow_heading(s, sx, sy, ux, uy, w, h, self, r) + plan.turn);
    const int dx = hd[0], dy = hd[1];

    if (plan.thicken && thick_enough(s, sx, sy, dx, dy, w, h, self, r, lift + plan.site)) {
        return true;
    }

    /* Growth limited to tip; produces trees mostly underground. */
    const bool shoot = (plan.site == run - 1) && !plan.thicken;
    if (!grow_into(s, sx + dx, sy + dy, dx, dy, w, h, self, shoot)) {
        return true;
    }
    spend_soil_moisture(s, w, r, soil_at, 1, contact_at, root_depth);

    if (r->hardens_to == 0 || r->harden_run == 0) {
        return true;
    }
    harden_stem(s, x, y, ux, uy, w, h, self, r);
    return true;
}
