/*
 * sand_reactions - fire chemistry: ignites fuel, spreads, is extinguished,
 * burns out. docs/sand/Sand-Simulation.md's "Fire chemistry" section carries
 * the design - why a lit log stays put instead of becoming fire, why steam and
 * smoke are separate materials, and why burying a log in sand will not put it
 * out.
 *
 * Both burning materials are dispatched by reaction_t.burns rather than a
 * CELL_MATERIAL(c) == MAT_FIRE test, which would silently ignore ember.
 *
 * The scan reads row[x] fresh at every index, so a cell ignited ahead of the
 * pointer within one pass spreads further in that same step while one ignited
 * behind it waits for the next. Deliberate - explosion-like spread through a
 * connected pocket of fuel rather than a slow creep - and true only for
 * pockets laid out ahead of the scan's fixed row-major direction.
 *
 * conduct_heat()'s reach attenuates with thickness rather than stopping at one
 * conductor cell: the pour brush cannot draw a wall one cell thick, so a
 * reach-of-one boiler is unbuildable on the device however clean it reads in
 * isolation. CONDUCT_REACH bounds the walk's cost, and must stay generous
 * enough that attenuation rather than the cap is what limits depth.
 */

#include "reaction_doc.h"
#include "sand_limits.h"
#include "sand_priv.h"

/* PAIR_BITS - classifies neighbour probes, replacing s->heat_mask/s->wet_mask
 * and adding three more. */

/* HONESTY: All but PAIR_DENSER use `theirs`. See top comment and
 * docs/sand/Reaction-Table.md. */

/* Consistent lookup shape for every consumer. PAIR_DENSER is genuinely
 * pairwise. */

/* See pair_theirs_bits() for `mine`-less calls. */
#define PAIR_HEAT_RESPONSIVE (1u << 0) /* theirs could pass try_heat_transform()'s first two gates - was heat_mask */
#define PAIR_WETS            (1u << 1) /* theirs is a liquid whose reaction row wets - was wet_mask */
#define PAIR_IGNITABLE       (1u << 2) /* theirs has a nonzero flammability - try_ignite()'s own first reject */
#define PAIR_QUENCHES        (1u << 3) /* theirs is a liquid that is neither fuel nor a heat source - neighbor_quenches() */
#define PAIR_DISSOLVABLE     (1u << 4) /* theirs has a nonzero dissolvable - step_one_dissolver_cell()'s own reject */
#define PAIR_CONDUCTS        (1u << 5) /* theirs has a nonzero conducts - conduct_heat()'s own reject */

/* Every pair bit any material now on the board can offer a neighbour: the OR
 * of theirs_bits over s->may_have_materials, recomputed once a step in
 * sand_step_reactions(). Every "is there anything here I could act ON" reject
 * reads this instead of walking to find out.
 *
 * Starts all-ones so nothing is skipped before the first pass has looked.
 *
 * seen_materials is the same mask being rebuilt from what THIS pass actually
 * walks, so the board can narrow as well as widen - a latch alone only ever
 * grows. */
static uint8_t present_pair_bits = 0xFFu;
static uint16_t seen_materials;

/* Can an acid-rain quad exist at all? It needs all four cells to be steam or
 * gas with exactly two steam - so two of each - and a board missing either
 * material can never form one, wherever the cells happen to sit.
 *
 * True until the first pass has looked, since it gates work being skipped. */
static bool acid_rain_possible = true;

/* The densest non-liquid anywhere on the board, from the same mask. A cell at
 * or above it has no possible smotherer, because neighbor_smothers() asks only
 * whether the NEIGHBOUR is a denser non-liquid - so the answer is a property of
 * the board, not of the cell asking, exactly as the pair bits are.
 *
 * 255 until the first pass has looked: it gates work being skipped. */
static uint8_t max_smothering_density = 255u;

/* A 17th material would fall out of the mask silently, and a material missing
 * from it reads as absent - which SKIPS work rather than adding it. Wrong
 * output, no crash, so nothing else would catch it. */
_Static_assert(MATERIAL_MAX <= 16, "may_have_materials is a uint16_t bit per material");

/* Two tables, because the key is NOT the material nibble - MAT_EXTENDED's
 * sixteen codes carry sixteen different reaction rows behind one nibble
 * value. Static: a burning cell reads its row across calls that an extern
 * would let the compiler suspect of writing it. */
static burn_plan_t material_plan[MATERIAL_MAX];
static burn_plan_t extended_plan[MATERIAL_EXTENDED_CODES];

static uint8_t pair_bits[MATERIAL_MAX][MATERIAL_MAX];

/* Reads theirs-only bits. Used by try_heat_transform(), step_one_cold_cell(),
 * conduct_heat(). MAT_EMPTY stores theirs-only bits. Avoid PAIR_DENSER. */
static inline uint8_t
pair_theirs_bits(uint8_t theirs) {
    return pair_bits[MAT_EMPTY][theirs];
}

static inline bool
neighbor_quenches(const sand_t* s, int nx, int ny, int w, int h) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    return (pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_QUENCHES) != 0;
}

/* Checks burial; returns false on failure. No cover_mask()/covered_at().
 * Burial skips rotation, side. */
static inline bool
smothered(const sand_t* s, int x, int y, int w, int h, uint8_t density) {
    /* Four walks only - here, conduct_heat(), and step_one_burning_cell()'s
     * quench and pair walks. On all seventeen it costs campfire 2.1% and
     * gunpowder 1.1% to buy the plant scenes 2%.
     *
     * It does not remove the table loads; the function more than doubles in
     * instructions and gains them. The win is straight-line paths on a core
     * with no branch predictor. */
#pragma GCC unroll 4
    for (int d = 0; d < 4; d++) {
        if (!neighbor_smothers(s, x + reaction_dirs[d][0], y + reaction_dirs[d][1], w, h, density)) {
            return false;
        }
    }
    return true;
}

static inline bool
touches_air(const sand_t* s, int x, int y, int w, int h) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (CELL_IS_EMPTY(n) || material_of(n)->kind == KIND_GAS) {
            return true;
        }
    }
    return false;
}

static void crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into);
static void crack_run_or_defer(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into);
static void cool_off_chain_or_defer(sand_t* s, int x, int y, int w, int h, uint8_t product, int chance);

static inline bool emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec);

static inline __attribute__((always_inline)) bool try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h,
                                                                           size_t at, cell_t n);

/* HEAT LEVELS DO NOT WAKE: a write that only moves a cell's heat nibble one
 * step marks its row for drawing and stops there.
 *
 * Waking buys another chance to MOVE, and only STONE and GLASS hold a
 * heat_ramp - both KIND_STATIC. What does change how a cell moves changes its
 * MATERIAL, through place_cell(), which still wakes.
 *
 * Waking shook a solid ice block out of its column, and put snow's crust rate
 * under COLD_REWARM_PERIOD: at a period of 1 the balance ceiling moved 9x. */

#define HEAT_FLAW_CLUMP 5

/* Picks heats_to or flaw_to. s->heat_flaw_seq/is_flawed are step-wide
 * mutable state shared board-wide - a race for two cores, so a split call
 * draws its own hashed roll instead: flaws land independently rather than
 * in fives, a cosmetic loss on the one material with a flaw row. */
static inline material_id_t
resolve_heat_flaw_yield(sand_t* s, int nx, int ny, const reaction_t* r) {
    if (r->flaw_to == 0) {
        return (material_id_t)r->heats_to;
    }
    bool flawed;
    if (s->rng_hashed) {
        flawed = sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_FLAW, r->flaw_chance);
    } else {
        if (s->heat_flaw_seq % HEAT_FLAW_CLUMP == 0) {
            s->heat_flaw_is_flawed = (int)(rng_next(&s->rng) & 0xFF) < r->flaw_chance;
        }
        s->heat_flaw_seq++;
        flawed = s->heat_flaw_is_flawed;
    }
    return flawed ? (material_id_t)r->flaw_to : (material_id_t)r->heats_to;
}

/* FORCED INLINE, and that is a performance fix rather than a
 * preference. */

/* Flaw/spoils branches grow function; unmeasured. Check cost if ratio
 * changes. */

/* Inline costs more than call on this chip. Forces four call sites. Capture
 * pending. */

/* Revert if host disagrees */

/* Turns (nx, ny) into heats_to if in bounds and roll succeeds. Heat without
 * burning. */

/* Sand into glass is the only use. Kept separate from try_ignite_given(). */

/* Wrapper checks before calling core; returns change status. */
static inline __attribute__((always_inline)) bool
try_heat_transform(sand_t* s, int nx, int ny, int w, int h) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
    const cell_t n = s->cells[at];
    if (CELL_IS_EMPTY(n)) {
        return false;
    }
    if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_HEAT_RESPONSIVE) == 0) {
        return false;
    }
    return try_heat_transform_given(s, nx, ny, w, h, at, n);
}

/* FORCED INLINE for 2 sites; wrapper perf. Shared walk confirms
 * step_one_burning_cell() shrink. */
static inline __attribute__((always_inline)) bool
try_heat_transform_given(sand_t* s, int nx, int ny, int w, int h, size_t at, cell_t n) {
    const reaction_t* r = reaction_of(n);

    /* Material climbs, transforms at top. Triggered by burning cell or
     * conductor. */
    if (r->heat_ramp != 0) {
        /* SHOCK: hot-to-cold, cracks. Mirror step_one_cold_cell(). Check
         * before ramp. */
        REACTION_DOC(shatters_to, "if warmed while badly chilled");
        if (r->shatters_to != 0 && CELL_VARIANT(n) <= SAND_SHOCK_COLD) {
            crack_run_or_defer(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n), (material_id_t)r->shatters_to);
            return true;
        }
        if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_HEAT_RAMP, r->heat_ramp)) {
            return false;
        }
        const uint8_t heat = CELL_VARIANT(n);
        if (heat + 1 >= MATERIAL_VARIANTS) {
            if (r->heats_to == 0) {
                return false; /* banks heat but melts into nothing */
            }
            place_reacted(s, nx, ny, at, (material_id_t)r->heats_to);
            return true;
        }
        s->cells[at] = CELL_MAKE(CELL_MATERIAL(n), heat + 1);
        s->may_have_temperature = true;
        mark_rows(s, nx, ny, ny); /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */
        return true;
    }

    if (r->heats_to == 0 || r->heat_chance == 0) {
        return false;
    }
    /* A LIT CELL `heats_to` GUNPOWDER_LIT_CELL. `explodes != 0` avoids extra
     * compare. */
    if (r->explodes != 0 && cell_code(n) >= r->lit_from) {
        return false;
    }
    if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_HEAT_CHANCE, r->heat_chance)) {
        return false;
    }

    /* WET EARTH FIRST. `dries != 0` checks. CELL_MOISTURE() confirms. Sand,
     * glass untouched. Two reads on success. Saturated dirt needs
     * SOIL_MOISTURE_MAX + 1 for metal with steam. */
    if (r->dries != 0 && moisture_of(n, r) != 0) {
        /* See reaction_t.spoils_to (material.h). Spoil pre-empts moisture
         * reduction. */

        /* Dirt dries ambiently, so cell may be below SOIL_MOISTURE_MAX before
         * HEAT. */

        /* Wet ore cracking on first contact, not after warning. */
        REACTION_DOC(spoils_to, "if wet when heat reaches it");
        if (r->spoils_to != 0 && sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_SPOILS, r->spoils_chance)) {
            place_reacted(s, nx, ny, at, (material_id_t)r->spoils_to);
            return true;
        }
        /* No neighbour to bias from - see soil_set_moisture() comment. */
        s->cells[at] = soil_set_moisture(n, (uint8_t)(moisture_of(n, r) - 1), 0);
        mark_rows(s, nx, ny, ny);
        wake_block_and_neighbors(s, nx, ny);
        emit_into_empty_neighbor(s, nx, ny, w, h, MAT_STEAM);
        return true;
    }

    place_reacted(s, nx, ny, at, resolve_heat_flaw_yield(s, nx, ny, r));
    return true;
}

/* Small fixed queues for the reaction split's LONG-REACH triggers - see
 * sand_step_reaction_reach() for why conduct_heat()/chilling/dissolving
 * need none at all. Under 1 KiB total, well below the sweep's own ~5 KiB
 * guard snapshot (sand.c): cap-limited events, never one entry per cell. */
#define REACT_EXPLOSION_DEFER_MAX 16
#define REACT_CRACK_DEFER_MAX     64
#define REACT_COOLOFF_DEFER_MAX   128

typedef struct {
    uint8_t x, y;
} react_coord_t;

typedef struct {
    uint8_t x, y, from, into;
} react_crack_defer_t;

typedef struct {
    uint8_t x, y, product;
    uint8_t chance;
} react_cooloff_defer_t;

static react_coord_t react_confined_ignite_defer[REACT_EXPLOSION_DEFER_MAX];
static uint8_t react_confined_ignite_defer_count;
static react_coord_t react_lava_burst_defer[REACT_EXPLOSION_DEFER_MAX];
static uint8_t react_lava_burst_defer_count;
static react_coord_t react_fuse_explosion_defer[REACT_EXPLOSION_DEFER_MAX];
static uint8_t react_fuse_explosion_defer_count;
static react_crack_defer_t react_crack_defer[REACT_CRACK_DEFER_MAX];
static uint8_t react_crack_defer_count;
static react_cooloff_defer_t react_cooloff_defer[REACT_COOLOFF_DEFER_MAX];
static uint8_t react_cooloff_defer_count;

/* Under 1 KiB total, well inside the sweep's own ~5 KiB guard-snapshot
 * precedent (sand.c) - these hold candidates for events that are already
 * rare and cap-limited, never one entry per cell. */
_Static_assert(3 * sizeof(react_coord_t) * REACT_EXPLOSION_DEFER_MAX + sizeof(react_crack_defer)
                       + sizeof(react_cooloff_defer)
                   <= 1024,
               "the reaction split's deferred queues must stay small - see the comment above");

static inline void
queue_confined_ignite(int x, int y) {
    if (react_confined_ignite_defer_count >= REACT_EXPLOSION_DEFER_MAX) {
        return; /* cap-limited already; a dropped candidate just never ignites this step */
    }
    react_confined_ignite_defer[react_confined_ignite_defer_count++] = (react_coord_t){(uint8_t)x, (uint8_t)y};
}

static inline void
queue_lava_burst(int x, int y) {
    if (react_lava_burst_defer_count >= REACT_EXPLOSION_DEFER_MAX) {
        return;
    }
    react_lava_burst_defer[react_lava_burst_defer_count++] = (react_coord_t){(uint8_t)x, (uint8_t)y};
}

static inline void
queue_fuse_explosion(int x, int y) {
    if (react_fuse_explosion_defer_count >= REACT_EXPLOSION_DEFER_MAX) {
        return;
    }
    react_fuse_explosion_defer[react_fuse_explosion_defer_count++] = (react_coord_t){(uint8_t)x, (uint8_t)y};
}

static inline void
queue_crack_run(int x, int y, material_id_t from, material_id_t into) {
    if (react_crack_defer_count >= REACT_CRACK_DEFER_MAX) {
        return; /* a rare hot/cold shock combo overflowing 64 in one step just never cracks this step */
    }
    react_crack_defer[react_crack_defer_count++] =
        (react_crack_defer_t){(uint8_t)x, (uint8_t)y, (uint8_t)from, (uint8_t)into};
}

static inline void
queue_cool_off_chain(int x, int y, uint8_t product, int chance) {
    if (react_cooloff_defer_count >= REACT_COOLOFF_DEFER_MAX) {
        return; /* the source cell already quenched locally; only the chain's further spread is lost */
    }
    react_cooloff_defer[react_cooloff_defer_count++] =
        (react_cooloff_defer_t){(uint8_t)x, (uint8_t)y, product, (uint8_t)chance};
}

#define CRACK_MAX 256

/* Cracks convert up to CRACK_MAX cells, leaving CULLET sand. */
static inline void
place_cracked(sand_t* s, int x, int y, size_t at, material_id_t into) {
    if (into == MAT_SAND) {
        place_cell(s, x, y, at, cullet_cell(s));
        return;
    }
    place_reacted(s, x, y, at, into);
}

static void
crack_run(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into) {
    uint16_t frontier[CRACK_MAX];
    int top = 0, done = 0;

    const size_t first = (size_t)y * (size_t)w + (size_t)x;
    place_cracked(s, x, y, first, into);
    frontier[top++] = (uint16_t)first;

    while (top > 0 && done < CRACK_MAX) {
        const uint16_t at = frontier[--top];
        const int cx = (int)(at % (unsigned)w);
        const int cy = (int)(at / (unsigned)w);
        done++;

        for (int d = 0; d < 4; d++) {
            const int nx = cx + reaction_dirs[d][0];
            const int ny = cy + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            if (CELL_MATERIAL(s->cells[nat]) != from) {
                continue;
            }
            place_cracked(s, nx, ny, nat, into);
            if (top < CRACK_MAX) {
                frontier[top++] = (uint16_t)nat;
            }
        }
    }
}

/* Walks lava-cooling chain, freezes neighbours holding same liquid. */

/* See KIND_LIQUID gate */

/* Iterative, not recursive. Stack limit on 368 KB RAM. */

static void
cool_off_chain(sand_t* s, int x, int y, int w, int h, uint8_t product, int chance) {
    int cx = x, cy = y;
    for (int link = 0; link < SAND_LAVA_COOLOFF_MAX_CHAIN; link++) {
        if (chance == 0 || (int)(rng_next(&s->rng) & 0xFF) >= chance) {
            return;
        }
        /* Collect eligible neighbours first for uniform pick. */
        int cand_x[4], cand_y[4], n_cand = 0;
        for (int d = 0; d < 4; d++) {
            const int nx = cx + reaction_dirs[d][0];
            const int ny = cy + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(n)) {
                continue;
            }
            if (material_of(n)->kind != KIND_LIQUID || reaction_of(n)->quench_to != product) {
                continue;
            }
            cand_x[n_cand] = nx;
            cand_y[n_cand] = ny;
            n_cand++;
        }
        if (n_cand == 0) {
            return;
        }
        const int pick = rng_below(&s->rng, n_cand);
        cx = cand_x[pick];
        cy = cand_y[pick];
        place_reacted(s, cx, cy, (size_t)cy * (size_t)w + (size_t)cx, product);
    }
}

static void
crack_run_or_defer(sand_t* s, int x, int y, int w, int h, material_id_t from, material_id_t into) {
    if (s->rng_hashed) {
        queue_crack_run(x, y, from, into);
        return;
    }
    crack_run(s, x, y, w, h, from, into);
}

static void
cool_off_chain_or_defer(sand_t* s, int x, int y, int w, int h, uint8_t product, int chance) {
    if (s->rng_hashed) {
        queue_cool_off_chain(x, y, product, chance);
        return;
    }
    cool_off_chain(s, x, y, w, h, product, chance);
}

/* `#define` used for materials with `dries != 0` */
#define SOIL_PERCOLATE_CHANCE 15

/* A saturated cell only rolls its conversion one step in this many.
 *
 * soaked_chance FLOORS AT 1 IN 256 - the roll is rng_next() & 0xFF - so it
 * cannot reach "magnitudes slower" alone. Spacing the roll can, and costs no
 * draw on the steps it skips. First oil went from 18 steps to ~400.
 *
 * A POWER OF TWO, the gate being a mask, so it moves only in factors of two.
 * Finer changes go on soaked_chance, which is gunpowder's alone - no other row
 * declares soaked_to, whatever an earlier note here claimed. */
#define SOAKED_CONVERT_PERIOD 64

/* Splits cell for input/output. Soaks UNIT, transforms or increases variant.
 * Drying decreases variant. Returns true if wet/near liquid. Prevents
 * `may_have_moisture`. Activated by SOAKING side. */
static bool
step_one_soaking_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t c = row[x];
    const uint8_t held = moisture_of(c, r);

    /* `>=` used, not `==`. Short-circuits on `soaked_to != 0`. */
    REACTION_DOC(soaked_to, "once fully saturated, at a per-step chance");
    const unsigned convert_period = (s->soak_convert > 0) ? (unsigned)s->soak_convert : SOAKED_CONVERT_PERIOD;
    if (r->soaked_to != 0 && held >= r->moist_max
        && (((unsigned)s->step_phase + (unsigned)x * 5u + (unsigned)y * 33u) & (convert_period - 1u)) == 0u
        && sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_SOAK_CONVERT, r->soaked_chance)) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, r->soaked_to);
        return true;
    }

    /* `soaks` below is forced to this same 0 by the override, so gaining or
     * sharing moisture can never fire - but AMBIENT drying (held != 0,
     * further down) does not read `soaks` at all, so it must not be
     * skipped here too. A dry cell with soaking off has nothing left this
     * stage can ever do to it. */
    if (s->soak == 0 && (r->dries == 0 || held == 0)) {
        return false;
    }

    bool beside_liquid = false;

    /* Cullet is glass milled back to grains, with none of a dune's pore
     * space left, so it neither drinks nor binds into soil. Forced to zero
     * rather than skipped at the conversion itself so a shard standing in
     * water does not spend the water for nothing. */
    const int soaks = cell_is_cullet(c) ? 0 : ((s->soak >= 0) ? s->soak : r->soaks);

    /* LOCAL, NOT BOARD-WIDE - any water this finds is in this block or one
     * touching it, exactly what BLOCK_LIQUID_NEAR covers. `moisture_capped`
     * excludes only `soaks_to == 0` (dirt): `soaks_to` materials (sand)
     * ignore `held` and must keep rolling toward their conversion. */
    const bool moisture_capped = r->soaks_to == 0 && held >= r->moist_max;
    if (soaks != 0 && r->soaks != 0 && !moisture_capped && liquid_near(s, x, y)) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t n = s->cells[nat];
            /* PAIR_WETS merges wets test into shift-and-test, covering
             * CELL_IS_EMPTY() without materials[] or reaction_of(n). */
            if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_WETS) == 0) {
                continue;
            }
            beside_liquid = true;

            if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_SOAK_WET_ROLL, soaks)) {
                continue;
            }
            /* The liquid pays for what was taken out of it. */
            pay_quench_cost(s, nx, ny, w);

            REACTION_DOC(soaks_to, "unless the grain is cullet, which is glass and holds no water");
            if (r->soaks_to != 0) {
                s->cells[(size_t)y * (size_t)w + (size_t)x] =
                    soil_cell(CELL_MAKE(r->soaks_to, 0), 0, 1, &reactions[r->soaks_to]);
                latch_content_flags(s, s->cells[(size_t)y * (size_t)w + (size_t)x]);
                mark_rows(s, x, y, y);
                wake_block_and_neighbors(s, x, y);
                mark_block_has_moisture(s, x, y);
                return true;
            }
            if (held < r->moist_max) {
                row[x] = with_moisture(c, (uint8_t)(held + 1), r);
                mark_rows(s, x, y, y);
                wake_block_and_neighbors(s, x, y);
                mark_block_has_moisture(s, x, y);
            }
            return true;
        }
    }

    const int spread = soaks;

    /* `dries` marks wet; `spread` checked for sinking. */
    if (r->dries != 0 && held >= 2 && spread != 0
        && sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_SOAK_SPREAD_GATE, spread)) {
        for (int d = 0; d < 4; d++) {
            const int nx = x + reaction_dirs[d][0];
            const int ny = y + reaction_dirs[d][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t n = s->cells[nat];
            if (CELL_IS_EMPTY(n)) {
                continue;
            }
            const reaction_t* nr = reaction_of(n);
            if (nr->soaks == 0 || cell_is_cullet(n)) {
                continue; /* not something that drinks */
            }

            int give, cost, recv_m;
            if (nr->soaks_to != 0) {
                give = held / 2;
                if (give == 0) {
                    continue; /* not enough to bind a grain */
                }
                cost = give;
                recv_m = give;
                s->cells[nat] = soil_cell(CELL_MAKE(nr->soaks_to, 0), 0, (uint8_t)give, &reactions[nr->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else if (same_species(n, c) && !cell_is_burning(n)) {
                /* Moisture_of() reads lit fuse as 0. Gap calc overwrites lit
                 * byte. */
                /* SIGNED ON PURPOSE, unlike the three halvings above: a
                 * WETTER neighbour makes this negative and the lines below
                 * depend on it, moving moisture the other way. Casting it
                 * unsigned turns a small negative into a huge positive. */
                give = (held - moisture_of(n, nr)) / 2;
                if (give == 0) {
                    continue; /* already even with this one */
                }
                recv_m = moisture_of(n, nr) + give;
                s->cells[nat] = with_moisture(n, (uint8_t)recv_m, nr);
                cost = give;
            } else {
                continue;
            }

            row[x] = soil_set_moisture(c, (uint8_t)(held - cost), (uint8_t)recv_m);
            mark_rows(s, x, y, y);
            mark_rows(s, nx, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            mark_block_has_moisture(s, x, y);
            mark_block_has_moisture(s, nx, ny);
            return true;
        }
    }

    /* Water percolates downhill, not just diffuses. */

    /* A fixed direction would advance a flat, evenly-damped sheet; a
     * wandering direction driving a real share down through the soil
     * instead produces fingers that split when a wet cell sends half one
     * way and half the other, and merge where two meet - closer to what
     * water actually does in sand. */

    /* Random number advances RNG for wet cells, shifts decisions. Trap
     * try_ignite() docs, once triggered. */
    if (r->dries != 0 && held != 0) {
        const int down = ring_of(s->last_load_dx, s->last_load_dy);
        int open[3], n_open = 0;
        for (int i = 0; i < 3; i++) {
            const int* fd = ring_dir(down + (i == 0 ? 0 : i == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                continue;
            }
            const cell_t below = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(below)) {
                continue;
            }
            const reaction_t* br = reaction_of(below);
            if (br->soaks == 0 || cell_is_cullet(below)) {
                continue;
            }
            /* Lit fuse carve-out: prevent dousing */
            if (!cell_is_burning(below)
                && (br->soaks_to != 0 || (br->dries != 0 && moisture_of(below, br) < br->moist_max))) {
                open[n_open++] = i;
            }
        }
        if (n_open != 0 && sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_SOAK_PERCOLATE, SOIL_PERCOLATE_CHANCE)) {
            const int pick = open[sand_rng_below_at(s, x, y, SAND_RNG_SLOT_REACT_SOAK_PERCOLATE_PICK, n_open)];
            const int* fd = ring_dir(down + (pick == 0 ? 0 : pick == 1 ? 1 : 7));
            const int nx = x + fd[0], ny = y + fd[1];
            const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
            const cell_t below = s->cells[nat];
            const reaction_t* br = reaction_of(below);

            int give = (held + 1) / 2;
            int cost = give;
            int recv_m;
            if (br->soaks_to != 0) {
                give = held / 2;
                if (give == 0) {
                    return true; /* too little to bind a grain */
                }
                cost = give;
                recv_m = give;
                /* Arrives WET, so no tone of its own - the soaking
                 * branch above's own comment covers why. */
                s->cells[nat] = soil_cell(CELL_MAKE(br->soaks_to, 0), 0, (uint8_t)give, &reactions[br->soaks_to]);
                latch_content_flags(s, s->cells[nat]);
            } else {
                const int room = (int)br->moist_max - moisture_of(below, br);
                if (give > room) {
                    give = room;
                }
                recv_m = moisture_of(below, br) + give;
                s->cells[nat] = with_moisture(below, (uint8_t)recv_m, br);
                cost = give;
            }
            row[x] = soil_set_moisture(c, (uint8_t)(held - cost), (uint8_t)recv_m);
            mark_rows(s, x, y, y);
            mark_rows(s, nx, ny, ny);
            wake_block_and_neighbors(s, x, y);
            wake_block_and_neighbors(s, nx, ny);
            mark_block_has_moisture(s, x, y);
            mark_block_has_moisture(s, nx, ny);
            return true;
        }
    }

    if (r->dries != 0 && held != 0 && sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_SOAK_DRY, r->dries)) {
        row[x] = soil_set_moisture(c, (uint8_t)(held - 1), 0);
        mark_rows(s, x, y, y);
        wake_block_and_neighbors(s, x, y);
        return held - 1 != 0;
    }

    /* r->dries halves wetness, prevents false "still wet." Without,
     * may_have_moisture could latch. */
    return (r->dries != 0 && held != 0) || beside_liquid;
}

/* may_have_heat_holder checks grid heat_ramp; skips heat_ramp == 0 cells. */
static void
step_one_warming_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const reaction_t* nr = reaction_of(n);

        /* Something that BANKS heat climbs one level. */
        if (nr->heat_ramp != 0) {
            const uint8_t t = CELL_VARIANT(n);
            if (t + 1 >= MATERIAL_VARIANTS) {
                continue; /* melting is the ramp's job, not convection's */
            }
            if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_WARM_BANK, r->warms)) {
                continue;
            }
            s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(t + 1));
            s->may_have_temperature = true;
            mark_rows(s, nx, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
            continue;
        }

        /* And something that CANNOT bank it melts outright. */

        /* Both gates. Gas must warm and target melt. Convection melts slower
         * than flame. */

        /* Warmer air costs snow its life. One gate would make every boiler a
         * snow-eater. */
        if (nr->chills == 0 || nr->heats_to == 0 || nr->heat_chance == 0) {
            continue;
        }
        if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_WARM_MELT_GATE, r->warms)) {
            continue;
        }
        /* Snow vital for shock. 1.5s smoke clears snow, risking boiler.
         * Quarter rate minimally impacts ice, doubles snow life. */
        if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_WARM_MELT, nr->heat_chance >> 2)) {
            continue;
        }
        place_reacted(s, nx, ny, nat, (material_id_t)nr->heats_to);
    }
}

/* COLD melts, pulls temp, cracks if hot. Chilling, melting snow. Warm liquid
 * aids survival. */
/* Bounds both conduction walks - conduct_heat() out of a burning cell and the
 * cold walk in step_one_cold_cell(). See "THE BOILER" for the rationale, and
 * note the two share it deliberately: a medium carries cold as far as it
 * carries heat. */
#define CONDUCT_REACH      32

/* How far COLD carries, which is no longer the same as heat.
 *
 * They shared CONDUCT_REACH on the argument that a medium carries cold as far
 * as it carries heat, and that stopped being true once cold got its own
 * attenuation run and, now, the diagonals. Thirty-two cells of cold read as
 * unrealistic in play - a third of it does not. */
#define COLD_REACH         (CONDUCT_REACH / 3)

/* Cells the cold crosses per attenuation roll - THE ONE PLACE COLD BEATS HEAT.
 * The heat walk rolls at every cell,
 * so at glass's conducts of 220 it clears CONDUCT_REACH about once in a
 * hundred; once per run of four makes that nearer one in three.
 *
 * Measured, 40x50 glass slab under snow at equilibrium: cells below ambient
 * 24% -> 64%, cells cold enough to shatter when warmed 12% -> 47%. */
#define COLD_CARRY_RUN     4

/* One carry attempt per cold cell every this many steps. The face a chiller
 * touches still cools every step; reaching deeper is rate-limited, so a slab
 * frosts over rather than reading as frozen the moment snow lands.
 *
 * A PERIOD, NOT A CHANCE: a 3-in-256 roll cost a draw on every cell every step
 * to say "no", and moved the shared RNG stream under every other rule on the
 * board. The x and y multipliers only spread the phase, so a drift narrower
 * than the period does not cool in one burst. Power of two. */
#define COLD_CARRY_PERIOD  512

/* A cell below ambient drifts back one level in this many steps.
 *
 * BOTH KNOBS ARE NEEDED: how deep the cold gets is the RATIO of cooling to
 * rewarming, not a race against time. Slowing the carry alone does not make a
 * slab take longer to freeze - it makes it never freeze.
 *
 * A period, not a divisor on the drain: cools is 5, so dividing lands on
 * 5, 2, 1, 0 and nothing between, and the only setting slower than a level a
 * step was never rewarming at all, which latches a slab cold forever. */
#define COLD_REWARM_PERIOD 32

static bool
step_one_cold_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    /* ONE DECISION FOR THE CELL, not one per direction - the question is
     * whether this cell sends cold onward this step, and asking it four times
     * would make the rate four times what it reads as. */
    const bool carries =
        (present_pair_bits & PAIR_CONDUCTS) != 0 && r->chills != 0
        && (((unsigned)s->step_phase + (unsigned)x * 5u + (unsigned)y * 33u) & (COLD_CARRY_PERIOD - 1u)) == 0u;
    bool spent_on_heat = false;

    /* THE CARRY RUNS ON ALL EIGHT, unlike the contact loop below it.
     *
     * Conduction is proximity, and a cell touching at a corner is as close as
     * one touching at a face - a cardinals-only walk sent cold down columns
     * and rows and left the diagonals of a slab untouched, which reads as a
     * grid rather than as cold spreading.
     *
     * Lifted out of that loop so it is walked ONCE for the cell rather than
     * once per cardinal neighbour, which also makes the source's bill below
     * one per step instead of up to four. */
    if (carries) {
        for (int d = 0; d < 8; d++) {
            const int* dir = ring_dir(d);
            int cx = x, cy = y;
            for (int depth = 1; depth < COLD_REACH; depth++) {
                cx += dir[0];
                cy += dir[1];
                if ((unsigned)cx >= (unsigned)w || (unsigned)cy >= (unsigned)h) {
                    break;
                }
                const size_t cat = (size_t)cy * (size_t)w + (size_t)cx;
                const cell_t cc = s->cells[cat];
                if (CELL_IS_EMPTY(cc)) {
                    break;
                }
                const reaction_t* cr = reaction_of(cc);
                if (cr->conducts == 0 || cr->heat_ramp == 0) {
                    break; /* the medium ends here */
                }
                const uint8_t ct = CELL_VARIANT(cc);
                if (ct == 0) {
                    continue; /* already as cold as the scale goes */
                }
                if ((depth % COLD_CARRY_RUN) == 0 && (int)(rng_next(&s->rng) & 0xFF) >= cr->conducts) {
                    break; /* the cold did not carry this far this step */
                }
                /* Drawn, not woken - see HEAT LEVELS DO NOT WAKE. This walk
                 * is where that rule was first found and paid for. */
                s->cells[cat] = CELL_MAKE(CELL_MATERIAL(cc), (uint8_t)(ct - 1));
                mark_rows(s, cx, cy, cy);
                if (ct > SAND_AMBIENT_HEAT) {
                    spent_on_heat = true;
                }
            }
        }
    }

    /* THE SOURCE PAYS FOR THE DEPTH TOO. Cooling something HOT has always cost
     * the chilling cell - that is what stops snow being a free and permanent
     * heat sink, and a test is named for it. Reaching deeper without paying
     * deeper would quietly void that. */
    if (spent_on_heat && try_heat_transform(s, x, y, w, h)) {
        return false;
    }

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        const reaction_t* nr = reaction_of(n);

        /* MELTING, from any liquid - see reaction_t.thaws. */
        if (r->thaws != 0 && r->heats_to != 0 && material_of(n)->kind == KIND_LIQUID
            && (int)(rng_next(&s->rng) & 0xFF) < r->thaws) {
            place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
            return false;
        }

        /* AND FROM WET SOIL, which is water too, just bound in grains. The
         * test above reads the neighbour's KIND, and dirt carries its water
         * as a moisture nibble, so a soaked bank looked dry to it.
         *
         * Scaled by wetness and halved: bound water reaches the snow more
         * slowly than free, and soil under about half saturation melts
         * nothing. The rate is computed BEFORE the roll so dry ground draws
         * no random number and cannot move the stream. */
        if (r->thaws != 0 && r->heats_to != 0 && nr->dries != 0 && nr->moist_max != 0) {
            const uint8_t wet = moisture_of(n, nr);
            const int rate = (int)r->thaws * (int)wet / ((int)nr->moist_max * 2);
            if (rate > 0 && (int)(rng_next(&s->rng) & 0xFF) < rate) {
                /* The soil pays for it, or one damp cell melts a whole bank. */
                s->cells[nat] = soil_set_moisture(n, (uint8_t)(wet - 1), 0);
                mark_rows(s, nx, ny, ny);
                place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, (material_id_t)r->heats_to);
                return false;
            }
        }

        if (r->chills == 0 || nr->heat_ramp == 0) {
            continue;
        }
        const uint8_t temp = CELL_VARIANT(n);

        REACTION_DOC(shatters_to, "if chilled while hot");
        if (temp >= SAND_SHOCK_HEAT && nr->shatters_to != 0) {
            crack_run(s, nx, ny, w, h, (material_id_t)CELL_MATERIAL(n), (material_id_t)nr->shatters_to);
            if (try_heat_transform(s, x, y, w, h)) {
                return false; /* and this cell melted paying for it */
            }
            continue;
        }

        if (temp == 0) {
            continue; /* the face is as cold as it goes - but the walk above
                       * has already carried cold past it */
        }
        if ((int)(rng_next(&s->rng) & 0xFF) >= r->chills) {
            continue;
        }

        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(temp - 1));
        s->may_have_temperature = true;
        mark_rows(s, nx, ny, ny); /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */

        /* AND ON THROUGH THE MEDIUM. Cold stopped where it touched: snow on
         * glass chilled three rows and sat there, the same at 250 steps as at
         * 1000.
         *
         * conduct_heat() has walked conductors all along but only out of a
         * BURNING cell, so no cold source could enter it. This is its mirror,
         * at the same reach, attenuating on the conductor's own `conducts` so
         * nothing new needs tuning. Free where nothing conducts. */

        if (temp > SAND_AMBIENT_HEAT && try_heat_transform(s, x, y, w, h)) {
            return false;
        }
    }

    /* THE SOURCE PAYS FOR THE DEPTH, ONCE PER STEP. Cooling something HOT has
     * always cost the chilling cell - that is what stops snow being a free and
     * permanent heat sink, and a test is named for it. The walk cools panes
     * several cells in, so billing only the face it touches would void that.
     *
     * Once per step, NOT per direction: per direction billed a block of ice up
     * to four melts a step and melted it out of the column it was put in. */
    if (spent_on_heat) {
        (void)try_heat_transform(s, x, y, w, h);
    }
    return true;
}

/* The carry walk above reaches up to COLD_REACH cells and stays entirely
 * serial rather than split: it decides whether to melt the SOURCE cell
 * before its own contact loop runs, and a deferred carry could only answer
 * that after the loop had already run for nothing. */
static bool
step_one_cold_cell_or_defer(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    if (s->rng_hashed) {
        return false;
    }
    return step_one_cold_cell(s, x, y, w, h, r);
}

#define SPREAD_SHIFT 1

static bool
step_one_tempered_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const cell_t c = row[x];
    const uint8_t temp = CELL_VARIANT(c);

    /* Spreading makes frost patch visible. */

    /* PUSHED to avoid ambient cell noticing frosted neighbour. Similar to
     * chilling bug. */

    /* Push question to where it is already cheap */
    bool wet = false;
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        /* PAIR_QUENCHES avoids reaction_of(n) load. */
        if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_QUENCHES) != 0) {
            wet = true;
        }
        if (r->conducts == 0 || reaction_of(n)->heat_ramp == 0) {
            continue;
        }
        const uint8_t nt = CELL_VARIANT(n);
        const int gap = (int)temp - (int)nt;
        if (gap > -2 && gap < 2) {
            continue;
        }
        if (!sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_TEMPER_SPREAD, r->conducts >> SPREAD_SHIFT)) {
            continue;
        }
        s->cells[nat] = CELL_MAKE(CELL_MATERIAL(n), (uint8_t)(gap > 0 ? nt + 1 : nt - 1));
        s->may_have_temperature = true;
        mark_rows(s, nx, ny, ny); /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */
    }

    /* MULTIPLIES DRAIN BY SAND_WET_COOLING_FACTOR (sand.h) */

    /* GATED TO temp > SAND_AMBIENT_HEAT - branch below untouched by `wet`. */

    /* Water cannot fall below ambient, preventing SAND_SHOCK_COLD. Snow
     * retains that role. */
    unsigned drain = r->cools;
    if (temp < SAND_AMBIENT_HEAT
        && (((unsigned)s->step_phase + (unsigned)x * 17u + (unsigned)y * 3u) & (COLD_REWARM_PERIOD - 1u)) != 0u) {
        drain = 0; /* not this cell's step to warm back up */
    }
    if (temp > SAND_AMBIENT_HEAT) {
        drain *= (unsigned)(temp - SAND_AMBIENT_HEAT);
        if (wet) {
            drain *= SAND_WET_COOLING_FACTOR;
        }
        if (drain > 255u) {
            drain = 255u;
        }
    }
    if (!sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_TEMPER_DRAIN, (int)drain)) {
        return temp != SAND_AMBIENT_HEAT;
    }

    const uint8_t next = (uint8_t)(temp > SAND_AMBIENT_HEAT ? temp - 1 : temp + 1);
    row[x] = CELL_MAKE(CELL_MATERIAL(c), next);
    mark_rows(s, x, y, y); /* drawn, not woken - see HEAT LEVELS DO NOT WAKE */
    return next != SAND_AMBIENT_HEAT;
}

/* How this cell is exposed: on a foreign face, on its own crust, or neither.
 *
 * A crust is a SHELL that GROWS INWARD, counted apart because the two run at
 * very different rates. A foreign face is where ice starts; a face on ice
 * already formed is how the shell thickens, far slower or the front eats the
 * bank.
 *
 * Neither face is interior, and never crusts. AIR IS NOT A FACE: count it and
 * a drift rims its whole outline in ice. Off-grid, likewise. */
#define FACE_FOREIGN        1u
#define FACE_CRUST          2u

/* How much crust a cell must be backed by before it joins one, over all eight
 * neighbours.
 *
 * EIGHT, BECAUSE FOUR CANNOT EXPRESS IT. A snow cell on a fully iced row has
 * one orthogonal ice neighbour, so any threshold above one stalls a flat front
 * and the cover never finishes. Over eight it has three, so three is the least
 * that advances a flat front while refusing a cell brushing a corner. */
#define CRUST_WIDEN_MIN_ICE 3

static inline unsigned
crust_faces(const sand_t* s, int x, int y, int w, int h, uint8_t mine, uint8_t becomes) {
    unsigned faces = 0;
    unsigned crust_seen = 0;
    bool open = false;
    for (int d = 0; d < 8; d++) {
        const int* dir = ring_dir(d);
        const int nx = x + dir[0];
        const int ny = y + dir[1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (CELL_IS_EMPTY(n)) {
            open = true;
            continue;
        }
        const uint8_t m = CELL_MATERIAL(n);
        if (m == becomes) {
            crust_seen++;
        } else if (m != mine && (dir[0] == 0 || dir[1] == 0)) {
            /* A FOREIGN face stays orthogonal: touching a wall at the corner
             * is not resting against it, and counting it ices the diagonal
             * staircase a poured pile leaves along any slope. */
            faces |= FACE_FOREIGN;
        }
    }
    /* THE RIM STAYS SNOW: open space anywhere around it makes this the drift's
     * own surface, and a surface does not thicken a crust forming underneath
     * it. Only widening is held to this - a cell pressed against a wall still
     * seeds, or a cover would never start. */
    if (!open && crust_seen >= CRUST_WIDEN_MIN_ICE) {
        faces |= FACE_CRUST;
    }
    return faces;
}

/* How often each of the two paths gets to roll. Periods, not divisors on the
 * chance: crusts is a small count, so dividing floors to zero.
 *
 * SEEDING NEEDS A CLOCK TOO. Ungated it rolls every step, so a whole contact
 * face turned within a second of settling while the shell behind it took
 * minutes - the crust appeared rather than formed. Still the faster of the
 * two, being what starts a shell, but no longer instant.
 *
 * The stagger multipliers differ per path so the two do not come due
 * together. */
#define CRUST_SEED_PERIOD             4
#define CRUST_WIDEN_PERIOD            8

/* Bounds burst cost per frame while retaining a paced chain. */
#define SAND_GUNPOWDER_BLAST_COOLDOWN 8
#define SAND_CONFINED_BLASTS_PER_STEP 4

/* Fixed blast radius. Cascade ignition simulates lid giving way. Tune on
 * device. */
#define SAND_GAS_IGNITE_BLAST_RADIUS  8

/* Shift for simplicity and reliability. */
#define SAND_DAMP_IGNITION_SHIFT      2

/* Checks GAS confinement by KIND_STATIC neighbours. Off-grid not considered a
 * wall. Avoids flood fill. */
static inline bool
gas_ignite_confined(const sand_t* s, int x, int y, int w, int h) {
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
        if (!CELL_IS_EMPTY(n) && material_of(n)->kind == KIND_STATIC) {
            return true;
        }
    }
    return false;
}

static inline bool
confined_blast_available(const sand_t* s) {
    return s->confined_blasts_this_step < SAND_CONFINED_BLASTS_PER_STEP;
}

static inline bool
ignite_confined_gas(sand_t* s, int nx, int ny) {
    if (!confined_blast_available(s)) {
        return false;
    }
    s->confined_blasts_this_step++;
    sand_explode(s, nx, ny, SAND_GAS_IGNITE_BLAST_RADIUS);
    return true;
}

/* The counter above is shared, step-wide state - a race for two cores. A
 * split call queues the cell and always reports success (bookkeeping only,
 * see react_burning_neighbors()); sand_step_reaction_reach() resolves the
 * cap for real, single core, once both phases have joined. */
static inline bool
ignite_confined_gas_or_defer(sand_t* s, int nx, int ny) {
    if (s->rng_hashed) {
        queue_confined_ignite(nx, ny);
        return true;
    }
    return ignite_confined_gas(s, nx, ny);
}

static inline bool
try_ignite_given(sand_t* s, int nx, int ny, int w, int h, size_t at, cell_t n) {
    const reaction_t* r = reaction_of(n);
    if (r->flammability == 0) {
        return false;
    }
    /* place_reacted() resets burn, prevents log extinguishing.
     * cell_is_burning() checks GUNPOWDER_LIT (7). */
    if (cell_is_burning(n)) {
        return false;
    }
    if (r->needs_air && !touches_air(s, nx, ny, w, h)) {
        return false; /* buried in more of itself - a pool of fuel burns
                         * at its surface, not through its volume */
    }
    /* s->flammability mirrors s->decay's override: negative uses material's
     * table, other values override; 255 is checked before rolling for
     * reproducibility. */
    int f = (s->flammability >= 0) ? s->flammability : r->flammability;
    /* Gunpowder dampens re-ignition odds. */
    REACTION_DOC(flammability, "damped further per moisture level, on any material with a moisture codec");
    const uint8_t m = (r->dries != 0) ? moisture_of(n, r) : 0;
    if (m) {
        f >>= SAND_DAMP_IGNITION_SHIFT * m;
    }
    if (f <= 0) {
        return false; /* before any RNG draw - a fully damped roll must not
                          shift the RNG stream for scenes that never reach
                          this material's moisture range */
    }
    if (f < 255 && !sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_IGNITE, f)) {
        return false;
    }
    if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS && gas_ignite_confined(s, nx, ny, w, h)) {
        return ignite_confined_gas_or_defer(s, nx, ny);
    }
    const material_id_t becomes = r->ignites_to ? r->ignites_to : MAT_FIRE;
    place_reacted(s, nx, ny, at, becomes);
    return true;
}

/* Bidirectional. Confirms placement. Checks for duplicates. */
static inline bool
emit_into_empty_neighbor(sand_t* s, int x, int y, int w, int h, uint8_t spec) {
    for (int d = 0; d < 4; d++) {
        const int fx = x + reaction_dirs[d][0];
        const int fy = y + reaction_dirs[d][1];
        if ((unsigned)fx >= (unsigned)w || (unsigned)fy >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)fy * (size_t)w + (size_t)fx;
        if (CELL_IS_EMPTY(s->cells[at])) {
            place_reacted(s, fx, fy, at, spec);
            return true;
        }
    }
    return false;
}

/* Lava LANDS as separate grains, spends steps in open air. */

/* MAT_FIRE cells burn, react, roll smoke; suspect in lava pours. */

/* SUPPORTED, matches sweep direction. Off-grid reads as STONE. */

/* Falling grain skips roll if cell moves next step. */

/* KIND_STATIC exempt; never moves, thus unaffected by gravity. */

/* Tries the cell against gravity, then its two neighbours, never sideways
 * or down. Not emit_into_empty_neighbor(): its screen-space order can put
 * fire beside/beneath lava and breaks under tilt. Not straight-up-only:
 * measured, 6% of rolls land vs 14% for this spread, costing the
 * thermal-shock scene 59% of its fire (827/2000 cells); raising `flare`
 * can't substitute - it sets a rate, not a density.
 *
 * Uses last_step, not last_load, matching try_flare()'s "below" check one
 * line earlier. */
static inline bool
emit_against_gravity(sand_t* s, int x, int y, int w, int h, uint8_t spec) {
    const int dx = s->last_step_dx, dy = s->last_step_dy;
    if (dx == 0 && dy == 0) {
        return false; /* no down yet, so no up to rise into */
    }
    const int up = ring_of(-dx, -dy);

    /* Straight on before either shoulder - the same ordering find_water()
     * uses when it walks gravity-ward. */
    for (int k = 0; k < 3; k++) {
        const int* d = ring_dir(up + (k == 0 ? 0 : k == 1 ? 1 : 7));
        const int ux = x + d[0], uy = y + d[1];
        if ((unsigned)ux >= (unsigned)w || (unsigned)uy >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)uy * (size_t)w + (size_t)ux;
        if (!CELL_IS_EMPTY(s->cells[at])) {
            continue;
        }
        place_reacted(s, ux, uy, at, spec);
        return true;
    }
    return false;
}

static inline bool
try_flare(sand_t* s, int x, int y, int w, int h, const material_t* mat, uint8_t flare) {
    if (flare == 0) {
        return false;
    }
    if (mat->kind != KIND_STATIC) {
        const cell_t below = sand_at(s, x + s->last_step_dx, y + s->last_step_dy);
        if (CELL_IS_EMPTY(below)) {
            return false;
        }
    }
    if (!sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_FLARE, flare)) {
        return false;
    }
    return emit_against_gravity(s, x, y, w, h, MAT_FIRE);
}

/* Attempts direct connection, fails. Rolls `conducts` to CONDUCT_REACH. Stops
 * on failure, off-grid, or empty. Liquid boils, fuel ignites, neighbors warm.
 * Returns status. */
static inline bool
conduct_heat(sand_t* s, int x, int y, int w, int h) {
    bool acted = false;

    /* SKIPPED WHOLE when nothing on the board conducts. Host counters: on a
     * full screen of fire this walk is entered 41216 times a step and finds
     * nothing every single time, and the phase split prices it at 36725 us,
     * 18% of that scene.
     *
     * RNG-NEUTRAL: the conduction roll sits inside the depth loop, which is
     * only reached once a neighbour has passed the PAIR_CONDUCTS reject - so
     * a board with no conductor draws nothing and the stream is untouched. */
    if ((present_pair_bits & PAIR_CONDUCTS) == 0) {
        return false;
    }

#pragma GCC unroll 4
    for (int d = 0; d < 4; d++) {
        const int dx = reaction_dirs[d][0];
        const int dy = reaction_dirs[d][1];
        int rx = x + dx;
        int ry = y + dy;

        if ((unsigned)rx >= (unsigned)w || (unsigned)ry >= (unsigned)h) {
            continue;
        }
        const cell_t first = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
        if (CELL_IS_EMPTY(first)) {
            continue;
        }
        /* Early-out - most neighbours are not conductors. Reading the bit
         * table rather than reaction_of()->conducts keeps the reject in
         * SRAM: reactions[] is flash-resident DROM behind the i-cache and
         * its 61-byte stride costs a multiply, which measured as 18% of a
         * fire step for four rejects that do nothing. */
        if ((pair_theirs_bits(CELL_MATERIAL(first)) & PAIR_CONDUCTS) == 0) {
            continue;
        }

        bool got_through = false;
        for (int depth = 0; depth < CONDUCT_REACH; depth++) {
            const cell_t here = s->cells[(size_t)ry * (size_t)w + (size_t)rx];
            /* The bit table folds all sixteen extended variants into one
             * MAT_EXTENDED slot, so the reject above passes any extended
             * cell once metal sets the bit. Re-testing here keeps that
             * approximation from reaching the roll below, which would spend
             * an RNG draw the exact test never spent. Dead for depth > 0:
             * the loop only advances into cells it has already found to
             * conduct. */
            const int own = reaction_of(here)->conducts;
            if (own == 0) {
                break;
            }
            const int c = (s->conduction >= 0) ? s->conduction : own;
            if ((int)(rng_next(&s->rng) & 0xFF) >= c) {
                break; /* heat stops inside this cell of the run */
            }

            const int nx = rx + dx;
            const int ny = ry + dy;
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
                break;
            }
            const cell_t next = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
            if (CELL_IS_EMPTY(next)) {
                break; /* never creates fire in empty space */
            }
            rx = nx;
            ry = ny;
            if (reaction_of(next)->conducts == 0) {
                got_through = true; /* the far side - not a conductor */
                break;
            }
            /* still inside the conductor run - loop again and roll for
             * THIS cell's own conducts figure */
        }

        if (!got_through) {
            continue;
        }

        const size_t bat = (size_t)ry * (size_t)w + (size_t)rx;
        const cell_t bc = s->cells[bat];
        const material_t* bm = material_of(bc);

        if (bm->kind == KIND_LIQUID && reaction_of(bc)->burns == 0 && reaction_of(bc)->flammability == 0) {
            /* Boils cells near conductor, replacing old method. Eliminates
             * deadlock via `boils` in reaction_t; retries on failure. */
            const int boils = (s->boils >= 0) ? s->boils : reaction_of(bc)->boils;
            if (boils != 0 && (int)(rng_next(&s->rng) & 0xFF) < boils) {
                /* place_reacted() avoids shortened steam life. */
                const uint8_t boils_to = reaction_of(bc)->boils_to ? reaction_of(bc)->boils_to : MAT_STEAM;
                place_reacted(s, rx, ry, bat, boils_to);
                acted = true;
            }
        } else {
            const reaction_t* br = reaction_of(bc);
            if (br->heat_ramp != 0 || (br->heats_to != 0 && br->heat_chance != 0)) {
                if (try_heat_transform(s, rx, ry, w, h)) {
                    acted = true;
                }
            }
            if (br->flammability != 0 && (!br->needs_air || touches_air(s, rx, ry, w, h))) {
                const uint8_t becomes = br->ignites_to ? br->ignites_to : MAT_FIRE;
                place_reacted(s, rx, ry, bat, becomes);
                acted = true;
            }
        }
    }

    return acted;
}

/* conduct_heat()'s walk reaches up to CONDUCT_REACH cells - never while
 * s->rng_hashed is armed. sand_step_reaction_reach() re-scans for every
 * still-burning cell once the local phase has settled, so no queue is
 * needed here at all. */
static bool
conduct_heat_or_defer(sand_t* s, int x, int y, int w, int h) {
    if (s->rng_hashed) {
        return false;
    }
    return conduct_heat(s, x, y, w, h);
}

/* ACID BUBBLES - CONTINUOUS, AMBIENT look, not event-specific. */

/* LIVES HERE, NOT IN move_liquid_grain() - skips settled blocks. */

/* NOT gated by block-sleeping - needed for dissolving, cooling, and bubbling. */

/* NO dx,dy PARAMETER - uses s->last_step_dx/last_step_dy for "which way is
 * down" (sand.h). */

/* UPWARD, STRAIGHT, MINIMAL SPREAD - NOT splash_displace()'s full ring. */
static void
acid_bubble(sand_t* s, int x, int y) {
    const int dx = s->last_step_dx, dy = s->last_step_dy;
    const int ux = x - dx, uy = y - dy; /* one step AGAINST gravity */
    if (!CELL_IS_EMPTY(sand_at(s, ux, uy))) {
        return; /* not exposed - nothing above to pop into */
    }
    if ((rng_next(&s->rng) & 0xFF) >= SAND_ACID_BUBBLE_CHANCE) {
        return;
    }
    const int i_up = (ring_of(dx, dy) + 4) & 7;
    const int spread = (int)(rng_next(&s->rng) % 3) - 1; /* -1, 0 or 1 */
    sand_impulse(s, x, y, (i_up + spread + 8) & 7, SAND_ACID_BUBBLE_SPEED);
}

/* A separate, much higher evaporate roll (below) can consume the whole
 * acid cell in one bite, so acid has a real chance to run out rather than
 * dissolving without limit while remaining one cell forever - the mistake
 * oil-soaked ash made before soaking became a real transfer. */

/* Acid eats one neighbour at a time. Returns whether it dissolved anything. */
static bool
step_one_dissolver_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r) {
    const bool per_material = s->evaporates < 0;
    const int evaporates = per_material ? r->evaporates : s->evaporates;
    if (evaporates != 0 && (int)(rng_next(&s->rng) & 0xFF) < evaporates
        && (!per_material || (rng_next(&s->rng) & 63u) == 0)) {
        const size_t at = (size_t)y * (size_t)w + (size_t)x;
        place_reacted(s, x, y, at, MAT_GAS);
        return true;
    }

    if ((int)(rng_next(&s->rng) & 0xFF) >= r->dissolves) {
        return false;
    }

    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t at = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[at];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        /* PAIR_DISSOLVABLE: rejects neighbours with dissolvable figure 0 -
         * genuine acid cells only. */
        if ((pair_theirs_bits(CELL_MATERIAL(n)) & PAIR_DISSOLVABLE) == 0) {
            continue;
        }
        /* Cullet is glass milled to grains, and MAT_GLASS has no
         * .dissolvable - acid cannot touch a pane whole. It shares
         * MAT_SAND's row, though, so the material table alone cannot say
         * cullet is different; reject it here explicitly. */
        if (cell_is_cullet(n)) {
            continue;
        }
        const uint8_t give = reaction_of(n)->dissolvable;
        if (give == 0 || (int)(rng_next(&s->rng) & 0xFF) >= give) {
            continue;
        }

        /* DILUTION occurs. SAND_ACID_DILUTE_TO_WATER_CHANCE dictates. Cells
         * evolve symmetrically. Winner vaporizes, loser converts.
         * CELL_VARIANT persists. Transformation costs. */
        if (CELL_MATERIAL(n) == MAT_WATER) {
            /* Measuring acid alone was asymmetrical: deep acid vs. adjacent
             * water. */
            int acid_backing = 0, water_backing = 0;
            for (int bd = 0; bd < 4; bd++) {
                const int abx = x + reaction_dirs[bd][0];
                const int aby = y + reaction_dirs[bd][1];
                if ((unsigned)abx < (unsigned)w && (unsigned)aby < (unsigned)h
                    && CELL_MATERIAL(s->cells[(size_t)aby * (size_t)w + (size_t)abx]) == MAT_ACID) {
                    acid_backing++;
                }

                const int wbx = nx + reaction_dirs[bd][0];
                const int wby = ny + reaction_dirs[bd][1];
                if ((unsigned)wbx < (unsigned)w && (unsigned)wby < (unsigned)h
                    && CELL_MATERIAL(s->cells[(size_t)wby * (size_t)w + (size_t)wbx]) == MAT_WATER) {
                    water_backing++;
                }
            }
            const int mass_bias =
                (s->acid_dilute_mass_bias >= 0) ? s->acid_dilute_mass_bias : SAND_ACID_DILUTE_MASS_BIAS;
            const int water_wins_chance = SAND_ACID_DILUTE_TO_WATER_CHANCE + (water_backing - acid_backing) * mass_bias;

            const int roll = (int)(rng_next(&s->rng) & 0xFF);
            const size_t self_at = (size_t)y * (size_t)w + (size_t)x;
            if (roll < SAND_ACID_DILUTE_EVAPORATE_CHANCE) {
                place_reacted(s, x, y, self_at, MAT_GAS);
            } else if (roll < SAND_ACID_DILUTE_EVAPORATE_CHANCE + water_wins_chance) {
                /* Acid converts to water, mass carries over. Uses
                 * place_cell()/place_reacted() for content flags. */
                place_cell(s, x, y, self_at, CELL_MAKE(MAT_WATER, CELL_VARIANT(row[x])));
                place_reacted(s, nx, ny, at, MAT_STEAM);
            } else {
                place_reacted(s, x, y, self_at, MAT_GAS);
                place_cell(s, nx, ny, at, CELL_MAKE(MAT_ACID, CELL_VARIANT(n)));
            }
            return true;
        }

        /* Oil turns to gas. Acid dies or pays quench cost. Rolls independent. */
        if (CELL_MATERIAL(n) == MAT_OIL) {
            const uint8_t oil_residue =
                ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_TO_GAS_CHANCE) ? MAT_GAS : MAT_ACID;
            place_reacted(s, nx, ny, at, oil_residue);

            if ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_OIL_DEATH_CHANCE) {
                const size_t self_at = (size_t)y * (size_t)w + (size_t)x;
                s->cells[self_at] = CELL_EMPTY;
                mark_rows(s, x, y, y);
                wake_block_and_neighbors(s, x, y);
            } else {
                pay_quench_cost(s, x, y, w);
            }
            return true;
        }

        /* Smoke lighter, try_bubble() moves it. Smoke or gas, 50% chance,
         * acid breathes gas. */
        if (r->fizz != 0 && (int)(rng_next(&s->rng) & 0xFF) < r->fizz) {
            const uint8_t residue = (rng_next(&s->rng) & 1) ? MAT_GAS : MAT_SMOKE;
            place_reacted(s, nx, ny, at, residue);
        } else {
            s->cells[at] = CELL_EMPTY;
            mark_rows(s, nx, ny, ny);
            wake_block_and_neighbors(s, nx, ny);
        }

        if ((int)(rng_next(&s->rng) & 0xFF) < SAND_ACID_EAT_DEATH_CHANCE) {
            row[x] = CELL_EMPTY;
            mark_rows(s, x, y, y);
            wake_block_and_neighbors(s, x, y);
        } else {
            pay_quench_cost(s, x, y, w);
        }
        return true;
    }
    return false;
}

/* acid_bubble()'s impulse and this walk's own multi-cell water/oil backing
 * check were never audited for stripe reach, so dissolving is left entirely
 * serial rather than split - sand_step_reaction_reach() re-scans for every
 * still-dissolving cell once the local phase has settled. */
static void
dissolver_and_bubble_or_defer(sand_t* s, uint8_t* row, int x, int y, int w, int h, const reaction_t* r, bool is_acid) {
    if (s->rng_hashed) {
        return;
    }
    if (is_acid) {
        acid_bubble(s, x, y);
    }
    step_one_dissolver_cell(s, row, x, y, w, h, r);
}

/* See reaction_t.explodes, material.h */

/* 2x2, NOT 3x3. Burn-out rolls independent per cell. */

/* gunpowder shares high nibble with extended statics; avoid counting ice as
 * fuse */

/* cell_is_burning(n): re-derive unnecessary */
static inline bool
lit_here(const sand_t* s, int nx, int ny, int w, int h, cell_t grain, const reaction_t* r) {
    if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
        return false;
    }
    const cell_t n = s->cells[(size_t)ny * (size_t)w + (size_t)nx];
    return same_species(n, grain) && cell_code(n) >= r->lit_from;
}

static inline bool
find_lit_two_by_two(const sand_t* s, int x, int y, int w, int h, cell_t grain, const reaction_t* r, int* out_dx,
                    int* out_dy) {
    for (int dy = -1; dy <= 1; dy += 2) {
        if (!lit_here(s, x, y + dy, w, h, grain, r)) {
            continue;
        }
        for (int dx = -1; dx <= 1; dx += 2) {
            if (lit_here(s, x + dx, y, w, h, grain, r) && lit_here(s, x + dx, y + dy, w, h, grain, r)) {
                *out_dx = dx;
                *out_dy = dy;
                return true;
            }
        }
    }
    return false;
}

static inline void
spend_lit_two_by_two(sand_t* s, int x, int y, int w, int dx, int dy) {
    const int px[3] = {x + dx, x, x + dx};
    const int py[3] = {y, y + dy, y + dy};
    for (int i = 0; i < 3; i++) {
        const size_t at = (size_t)py[i] * (size_t)w + (size_t)px[i];
        place_reacted(s, px[i], py[i], at, MAT_FIRE);
    }
}

typedef struct {
    sand_t* s;
    uint8_t* row;
    int y;
    int w;
    int h;
} reaction_row_t;

typedef struct {
    sand_t* s;
    const reaction_t* rx;
    const material_t* mat;
    cell_t grain;
    int x;
    int y;
} burning_cell_t;

static inline bool
tick_burning_cell(const reaction_row_t* reaction_row, int x, cell_t* grain, const reaction_t* rx,
                  const burn_plan_t* plan, uint8_t mat_id) {
    if ((plan->flags & BURN_LIT) != 0) {
        return tick_decay_at(reaction_row->s, reaction_row->row, x, reaction_row->y, grain, rx, plan->tick_rate);
    }
    return tick_decay(reaction_row->s, reaction_row->row, x, reaction_row->y, grain, mat_id, plan->tick_rate);
}

/* The explosive half of a burn-out - see finish_burning_cell(). Split out
 * so the common, non-exploding case never carries its weight, and so the
 * `_or_defer` gate below sits at one small call rather than growing the
 * caller. */
static __attribute__((noinline)) void
finish_exploding_burnout(const burning_cell_t* cell) {
    sand_t* const s = cell->s;
    const reaction_t* const rx = cell->rx;
    const cell_t grain = cell->grain;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const int h = s->h;

    REACTION_DOC(
        explodes,
        "at burn-out, if it is one corner of a 2x2 that is all lit and the board's blast cooldown has run out");
    int dx = 0, dy = 0;
    if (s->impulse_buf != NULL && s->fuse_blast_wait == 0 && find_lit_two_by_two(s, x, y, w, h, grain, rx, &dx, &dy)) {
        spend_lit_two_by_two(s, x, y, w, dx, dy);
        if (s->rng_hashed) {
            /* s->fuse_blast_wait is shared, mutable, step-wide state - the
             * cooldown write and the explosion both move to the serial
             * reach pass, where only one core is ever running. */
            queue_fuse_explosion(x, y);
        } else {
            s->fuse_blast_wait = (uint8_t)((s->fuse_cooldown >= 0) ? s->fuse_cooldown : SAND_GUNPOWDER_BLAST_COOLDOWN);
            sand_explode(s, x, y, rx->explodes);
        }
        return;
    }
    place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, MAT_FIRE);
}

static __attribute__((noinline)) bool
finish_burning_cell(const burning_cell_t* cell) {
    const reaction_t* const rx = cell->rx;

    if (rx->explodes == 0) {
        sand_t* const s = cell->s;
        const int x = cell->x;
        const int y = cell->y;
        const size_t at = (size_t)y * (size_t)s->w + (size_t)x;
        const uint8_t residue = rx->residue;
        if (residue != 0 && sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_BURN_RESIDUE, residue)) {
            place_reacted(s, x, y, at, MAT_SMOKE);
        }
        return true;
    }

    finish_exploding_burnout(cell);
    return true;
}

static __attribute__((noinline)) void
quench_lit_cell(const burning_cell_t* cell) {
    sand_t* const s = cell->s;
    const reaction_t* const rx = cell->rx;
    const cell_t grain = cell->grain;
    const int x = cell->x;
    const int y = cell->y;
    const size_t at = (size_t)y * (size_t)s->w + (size_t)x;

    if (rx->tones != 0) {
        place_cell(s, x, y, at, with_moisture(grain, rx->moist_max, rx));
        return;
    }
    s->cells[at] = CELL_MAKE(CELL_MATERIAL(grain), 0);
    mark_rows(s, x, y, y);
    wake_block_and_neighbors(s, x, y);
}

static __attribute__((noinline)) bool
quench_product(const burning_cell_t* cell, int nx, int ny, uint8_t* product) {
    const int w = cell->s->w;
    if (CELL_MATERIAL(cell->grain) != MAT_FIRE) {
        return true;
    }
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    const uint8_t liquid_boils_to = reaction_of(cell->s->cells[nat])->boils_to;
    if (liquid_boils_to != MAT_GAS) {
        *product = liquid_boils_to ? liquid_boils_to : MAT_STEAM;
        return true;
    }
    const bool leaves_residue =
        sand_rng_chance_at(cell->s, nx, ny, SAND_RNG_SLOT_REACT_QUENCH_RESIDUE, SAND_ACID_QUENCH_RESIDUE_CHANCE);
    *product = sand_rng_chance_at(cell->s, nx, ny, SAND_RNG_SLOT_REACT_QUENCH_SMOKE, SAND_ACID_QUENCH_SMOKE_CHANCE)
                   ? MAT_SMOKE
                   : MAT_GAS;
    return leaves_residue;
}

static __attribute__((noinline)) void
quench_unlit_cell(const burning_cell_t* cell, int nx, int ny) {
    sand_t* const s = cell->s;
    const reaction_t* const rx = cell->rx;
    const material_t* const mat = cell->mat;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const uint8_t quench_to = rx->quench_to;

    if (quench_to == 0) {
        s->cells[at] = CELL_EMPTY;
        mark_rows(s, x, y, y);
        wake_block_and_neighbors(s, x, y);
        return;
    }

    uint8_t product = quench_to;
    if (!quench_product(cell, nx, ny, &product)) {
        s->cells[at] = CELL_EMPTY;
        mark_rows(s, x, y, y);
        wake_block_and_neighbors(s, x, y);
        return;
    }

    place_reacted(s, x, y, at, product);
    if (mat->kind == KIND_LIQUID) {
        const int lava_cooloff = (s->lava_cooloff >= 0) ? s->lava_cooloff : SAND_LAVA_COOLOFF_CHANCE;
        cool_off_chain_or_defer(s, x, y, w, s->h, product, lava_cooloff);
    }
}

static inline __attribute__((always_inline)) bool
quench_burning_cell(const burning_cell_t* cell, bool lit_state) {
    sand_t* const s = cell->s;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const int h = s->h;

#pragma GCC unroll 4
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if (!neighbor_quenches(s, nx, ny, w, h)) {
            continue;
        }
        if (lit_state) {
            quench_lit_cell(cell);
        } else {
            quench_unlit_cell(cell, nx, ny);
        }
        pay_quench_cost(s, nx, ny, w);
        return true;
    }
    return false;
}

static __attribute__((noinline)) void
smother_burning_cell(const burning_cell_t* cell, bool lit_state) {
    sand_t* const s = cell->s;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const size_t at = (size_t)y * (size_t)w + (size_t)x;

    s->cells[at] = lit_state ? cell_with_code(cell->grain, 0) : CELL_EMPTY;
    mark_rows(s, x, y, y);
    wake_block_and_neighbors(s, x, y);
}

static __attribute__((noinline)) bool
try_lava_burst(const burning_cell_t* cell) {
    sand_t* const s = cell->s;
    const reaction_t* const rx = cell->rx;
    const material_t* const mat = cell->mat;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const int h = s->h;

    if (!confined_blast_available(s)) {
        return false;
    }
    const bool burst_natural = s->lava_burst < 0;
    const int burst_chance = burst_natural ? SAND_LAVA_BURST_CHANCE : s->lava_burst;
    if (burst_chance != 0 && (int)(rng_next(&s->rng) & 0xFF) < burst_chance
        && (!burst_natural || (rng_next(&s->rng) % SAND_LAVA_BURST_GATE) == 0)
        && covered_at(s, x, y, w, h, mat->density)) {
        place_reacted(s, x, y, (size_t)y * (size_t)w + (size_t)x, rx->quench_to);
        s->confined_blasts_this_step++;
        sand_explode(s, x, y, SAND_LAVA_BURST_RADIUS);
        return true;
    }
    return false;
}

/* try_lava_burst() reads and bumps the shared confined-blast counter and
 * calls sand_explode() - a stripe-parallel call queues the candidate
 * instead and reports "did not burst", the same as a roll that missed;
 * sand_step_reaction_reach() runs the real check afterward, single core. */
static bool
try_lava_burst_or_defer(const burning_cell_t* cell) {
    if (cell->s->rng_hashed) {
        queue_lava_burst(cell->x, cell->y);
        return false;
    }
    return try_lava_burst(cell);
}

typedef enum {
    BURN_NEIGHBOR_NONE,
    BURN_NEIGHBOR_ACTED,
    BURN_NEIGHBOR_SOURCE_QUENCHED,
} burn_neighbor_result_t;

static __attribute__((noinline)) burn_neighbor_result_t
react_burning_heat_neighbor(const burning_cell_t* cell, int nx, int ny, cell_t n, int lava_cooloff) {
    sand_t* const s = cell->s;
    const reaction_t* const rx = cell->rx;
    const material_t* const mat = cell->mat;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const int h = s->h;
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
    const uint8_t before_mat = CELL_MATERIAL(n);
    bool changed = try_heat_transform_given(s, nx, ny, w, h, nat, n);
    if (!changed && mat->kind == KIND_LIQUID) {
        const reaction_t* const nr = reaction_of(n);
        if (nr->melts != 0 && nr->heats_to != 0 && sand_rng_chance_at(s, nx, ny, SAND_RNG_SLOT_REACT_MELT, nr->melts)) {
            place_reacted(s, nx, ny, nat, nr->heats_to);
            changed = true;
        }
    }
    if (!changed) {
        return BURN_NEIGHBOR_NONE;
    }
    if (lava_cooloff != 0 && CELL_MATERIAL(s->cells[nat]) != before_mat
        && sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_LAVA_COOLOFF_TRIGGER, lava_cooloff)) {
        place_reacted(s, x, y, at, rx->quench_to);
        cool_off_chain_or_defer(s, x, y, w, h, rx->quench_to, lava_cooloff);
        return BURN_NEIGHBOR_SOURCE_QUENCHED;
    }
    return BURN_NEIGHBOR_ACTED;
}

static inline __attribute__((always_inline)) burn_neighbor_result_t
react_burning_neighbors(const burning_cell_t* cell, int lava_cooloff) {
    sand_t* const s = cell->s;
    const int x = cell->x;
    const int y = cell->y;
    const int w = s->w;
    const int h = s->h;
    const uint8_t* const my_pair_row = pair_bits[CELL_MATERIAL(cell->grain)];
    bool acted = false;

#pragma GCC unroll 4
    for (int d = 0; d < 4; d++) {
        const int nx = x + reaction_dirs[d][0];
        const int ny = y + reaction_dirs[d][1];
        if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= (unsigned)h) {
            continue;
        }
        const size_t nat = (size_t)ny * (size_t)w + (size_t)nx;
        const cell_t n = s->cells[nat];
        if (CELL_IS_EMPTY(n)) {
            continue;
        }
        if (s->impulse_buf != NULL && material_of(n)->kind == KIND_GAS && gas_ignite_confined(s, nx, ny, w, h)
            && !confined_blast_available(s)) {
            continue;
        }
        const uint8_t pair = my_pair_row[CELL_MATERIAL(n)];
        acted |= (pair & PAIR_IGNITABLE) != 0 && try_ignite_given(s, nx, ny, w, h, nat, n);
        const burn_neighbor_result_t heat_result = (pair & PAIR_HEAT_RESPONSIVE) != 0
                                                       ? react_burning_heat_neighbor(cell, nx, ny, n, lava_cooloff)
                                                       : BURN_NEIGHBOR_NONE;
        acted |= heat_result != BURN_NEIGHBOR_NONE;
        if (heat_result == BURN_NEIGHBOR_SOURCE_QUENCHED) {
            return BURN_NEIGHBOR_SOURCE_QUENCHED;
        }
    }
    return acted ? BURN_NEIGHBOR_ACTED : BURN_NEIGHBOR_NONE;
}

static bool
step_one_burning_cell(const reaction_row_t* reaction_row, int x, cell_t grain, const reaction_t* rx,
                      const burn_plan_t* plan) {
    sand_t* const s = reaction_row->s;
    const int y = reaction_row->y;
    const int w = reaction_row->w;
    const int h = reaction_row->h;
    const material_t* mat = material_of(grain);
    const uint8_t mat_id = CELL_MATERIAL(grain);
    const uint8_t plan_flags = plan->flags;
    const bool lit_state = (plan_flags & BURN_LIT) != 0;

    if (!tick_burning_cell(reaction_row, x, &grain, rx, plan, mat_id)) {
        return finish_burning_cell(&(burning_cell_t){s, rx, mat, grain, x, y});
    }

    const burning_cell_t cell = {s, rx, mat, grain, x, y};

    if (s->may_have_liquid && quench_burning_cell(&cell, lit_state)) {
        return true;
    }

    if ((plan_flags & BURN_SMOTHERS) != 0 && smothered(s, x, y, w, h, mat->density)) {
        smother_burning_cell(&cell, lit_state);
        return true;
    }

    bool acted = false;
    const bool is_lava = (plan_flags & BURN_LAVA) != 0;
    if (is_lava && try_lava_burst_or_defer(&cell)) {
        return true;
    }

    const int lava_cooloff = is_lava ? ((s->lava_cooloff >= 0) ? s->lava_cooloff : SAND_LAVA_COOLOFF_CHANCE) : 0;
    const burn_neighbor_result_t neighbor_result = (present_pair_bits & (PAIR_IGNITABLE | PAIR_HEAT_RESPONSIVE)) != 0
                                                       ? react_burning_neighbors(&cell, lava_cooloff)
                                                       : BURN_NEIGHBOR_NONE;
    acted |= neighbor_result != BURN_NEIGHBOR_NONE;
    if (neighbor_result == BURN_NEIGHBOR_SOURCE_QUENCHED) {
        return true;
    }

    if (conduct_heat_or_defer(s, x, y, w, h)) {
        acted = true;
    }

    /* The plan's copy, not reaction_of(grain)->flare: decay only ever rewrites
     * the code nibble, so the row is the same one and re-deriving it costs a
     * flash dereference per burning cell for nothing. */
    if (try_flare(s, x, y, w, h, mat, plan->flare)) {
        acted = true;
    }

    return acted;
}

/* Tiny chance for cells to merge. Called per cell, L-to-R, T-to-B. */
static inline bool
step_one_condensing_cell(sand_t* s, int x, int y, int w, int h, const reaction_t* r) {
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r = at + 1;
    const size_t at_d = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t mat_id = CELL_MATERIAL(s->cells[at]);

    if (CELL_IS_EMPTY(s->cells[at_r]) || CELL_MATERIAL(s->cells[at_r]) != mat_id || CELL_IS_EMPTY(s->cells[at_d])
        || CELL_MATERIAL(s->cells[at_d]) != mat_id || CELL_IS_EMPTY(s->cells[at_dr])
        || CELL_MATERIAL(s->cells[at_dr]) != mat_id) {
        return false;
    }

    const int condenses = (s->condenses >= 0) ? s->condenses : r->condenses;
    if (!sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_CONDENSE, condenses)) {
        return false;
    }

    place_reacted(s, x, y, at, r->condenses_to);
    place_cell(s, x + 1, y, at_r, CELL_EMPTY);
    place_cell(s, x, y + 1, at_d, CELL_EMPTY);
    place_cell(s, x + 1, y + 1, at_dr, CELL_EMPTY);
    return true;
}

/* ACID RAIN fake collapse, MAT_GAS/STEAM, 2-2 splits. Survivor 50/50
 * Acid/Water. Caller: FOUND_DISSOLVER, FOUND_MOISTURE, FOUND_CONDENSING. */
static inline bool
step_one_acid_rain_cell(sand_t* s, int x, int y, int w, int h) {
    /* SKIPPED WHOLE when the board cannot hold a quad. Four cell loads and
     * four material decodes per gas or steam cell otherwise, every step, to
     * rediscover that the same thing is missing.
     *
     * RNG-NEUTRAL: the roll sits below the quad tests, so a board that cannot
     * form one draws nothing and the stream is untouched.
     *
     * Measured: a screen of smoke and steam reaches this 204247 times a run
     * and forms a quad NEVER - it holds no gas at all. */
    if (!acid_rain_possible) {
        return false;
    }
    if (x + 1 >= w || y + 1 >= h) {
        return false;
    }
    const size_t at = (size_t)y * (size_t)w + (size_t)x;
    const size_t at_r = at + 1;
    const size_t at_d = at + (size_t)w;
    const size_t at_dr = at_d + 1;
    const uint8_t m0 = CELL_MATERIAL(s->cells[at]);
    const uint8_t m1 = CELL_MATERIAL(s->cells[at_r]);
    const uint8_t m2 = CELL_MATERIAL(s->cells[at_d]);
    const uint8_t m3 = CELL_MATERIAL(s->cells[at_dr]);

    if ((m0 != MAT_STEAM && m0 != MAT_GAS) || (m1 != MAT_STEAM && m1 != MAT_GAS) || (m2 != MAT_STEAM && m2 != MAT_GAS)
        || (m3 != MAT_STEAM && m3 != MAT_GAS)) {
        return false;
    }
    const int steam_count = (m0 == MAT_STEAM) + (m1 == MAT_STEAM) + (m2 == MAT_STEAM) + (m3 == MAT_STEAM);
    if (steam_count != 2) {
        return false;
    }

    const int acid_rain = (s->acid_rain >= 0) ? s->acid_rain : SAND_ACID_RAIN_CHANCE;
    if (!sand_rng_chance_at(s, x, y, SAND_RNG_SLOT_REACT_ACID_RAIN_GATE, acid_rain)) {
        return false;
    }

    /* Coin flip, see header for acid probability. */
    const uint8_t residue =
        (sand_rng_next_at(s, x, y, SAND_RNG_SLOT_REACT_ACID_RAIN_RESIDUE) & 1) ? MAT_ACID : MAT_WATER;
    place_reacted(s, x, y, at, residue);
    place_cell(s, x + 1, y, at_r, CELL_EMPTY);
    place_cell(s, x, y + 1, at_d, CELL_EMPTY);
    place_cell(s, x + 1, y + 1, at_dr, CELL_EMPTY);
    return true;
}

/* Avoids treating stone as heat source */

/* PRESENCE, NOT ACTIVITY: may_have_burning latches as soon as a cell is
 * identified as burning, before step_one_burning_cell() runs - not only
 * when it reacts or decays this step. Latching on activity instead would
 * let a quiet burning cell (no fuel/liquid touching it, roll not hit) fall
 * out of the bookkeeping while still on the grid. */

/* Separate flags avoid false positives. */
#define FOUND_BURNING     1u
#define FOUND_DISSOLVER   2u
#define FOUND_TEMPERATURE 4u
#define FOUND_MOISTURE    8u
#define FOUND_FALLER      16u
#define FOUND_FALLER_MOVE 32u
#define FOUND_CONDENSING  64u

/* Cells the dispatch loop below actually visits, across both the full row
 * walk and the soak-only partial walk. Never reset here - see its own
 * comment in sand_priv.h. */
unsigned sand_reactions_cells_dispatched;

/* Which shape the call below took - see sand_priv.h. */
bool sand_reactions_last_was_soak_only;

/* REACTION-STAGE DISPATCH TABLE skips PREFIX rows. Water, oil, metal traverse
 * all fields.
 *
 * RANGE [x_lo, x_hi), NOT ALWAYS THE FULL ROW: the soak-only walk in
 * sand_step_reactions() calls this once per BLOCK_LIQUID_NEAR block instead
 * of once per row, ascending in x the same way a full [0, w) call would, so
 * a cell it does visit sees exactly the state and RNG stream a full walk
 * would have given it. */
static unsigned
step_one_reacting_row(sand_t* s, int y, int w, int h, int x_lo, int x_hi) {
    const size_t row_at = (size_t)y * (size_t)w;
    uint8_t* row = s->cells + row_at;
    const reaction_row_t reaction_row = {s, row, y, w, h};
    static void* const stage_labels[RSTAGE_COUNT] = {
        &&stage_burn_any, &&stage_burn_always, &&stage_burn_check, &&stage_dissolve, &&stage_acid_rain,
        &&stage_condense, &&stage_heat_ramp,   &&stage_crust,      &&stage_chill,    &&stage_warm,
        &&stage_soak_dry, &&stage_fall,        &&stage_drink,      &&stage_root,     &&stage_grow,
        &&stage_sprout,   &&stage_bud,         &&stage_end,
    };

    /* Once per call, not per cell: the loop visits every cell of the range,
     * and a store per cell would tax every pass in every build. */
    sand_reactions_cells_dispatched += (unsigned)(x_hi - x_lo);

    unsigned found = 0;
    for (int x = x_lo; x < x_hi; x++) {
        const cell_t c = row[x];
        seen_materials |= (uint16_t)(1u << CELL_MATERIAL(c));
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        const reaction_t* r;
        const burn_plan_t* plan;
        if (CELL_MATERIAL(c) == MAT_EXTENDED) {
            const uint8_t variant = CELL_VARIANT(c);
            r = &extended_reactions[variant];
            plan = &extended_plan[variant];
        } else {
            const uint8_t mat = CELL_MATERIAL(c);
            r = &reactions[mat];
            plan = &material_plan[mat];
        }
        goto* stage_labels[plan->stage];

    stage_burn_always:
        found |= FOUND_BURNING;
        step_one_burning_cell(&reaction_row, x, c, r, plan);
        continue;

    stage_burn_check:
        if (cell_code(c) >= r->lit_from) {
            found |= FOUND_BURNING;
            step_one_burning_cell(&reaction_row, x, c, r, plan);
            continue;
        }
        goto stage_dissolve;

    stage_burn_any:
        if (cell_is_burning(c)) {
            found |= FOUND_BURNING;
            step_one_burning_cell(&reaction_row, x, c, r, plan);
            continue;
        }

    stage_dissolve:
        if (r->dissolves) {
            found |= FOUND_DISSOLVER;
            /* MAT_ACID specific - see acid_bubble()'s comment. Future
             * dissolvers may not bubble. */
            dissolver_and_bubble_or_defer(s, row, x, y, w, h, r, CELL_MATERIAL(c) == MAT_ACID);
            continue;
        }
        /* See SAND_ACID_RAIN_CHANCE's comment (sand.h). */

        /* found |= ... survivor writes at (x, y), walk never revisits, no
         * other branch reports FOUND_DISSOLVER/FOUND_MOISTURE, steam can't
         * report FOUND_CONDENSING. */

    stage_acid_rain:
        if (CELL_MATERIAL(c) == MAT_GAS || CELL_MATERIAL(c) == MAT_STEAM) {
            if (step_one_acid_rain_cell(s, x, y, w, h)) {
                found |= FOUND_DISSOLVER | FOUND_MOISTURE | FOUND_CONDENSING;
                continue;
            }
        }
        /* May_have_temperature not re-armed for boards with no heat-holder. */
    stage_condense:
        if (r->condenses != 0) {
            found |= FOUND_CONDENSING;
            if (step_one_condensing_cell(s, x, y, w, h, r)) {
                continue;
            }
        }
    stage_heat_ramp:
        if (r->heat_ramp != 0) {
            if (CELL_VARIANT(c) != SAND_AMBIENT_HEAT && step_one_tempered_cell(s, row, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* Drift on dry ground persists, found later when liquid reaches it. */
    stage_crust:
        /* THE ROLL IS LAST on purpose: drawn before the settled test it would
         * shift the random stream for every scene whether or not snow is
         * present, moving every baselined hash for a rule that did nothing.
         *
         * Converts in place and deliberately does NOT wake. Waking would clear
         * BLOCK_SETTLED on the very bank whose stillness allowed this, so the
         * crust would form one cell and stall; and nothing needs waking,
         * because snow becoming ice only makes the board more solid. */
        const unsigned faces = (r->crusts != 0 && cell_settled(s, x, y))
                                   ? crust_faces(s, x, y, w, h, CELL_MATERIAL(c), CELL_MATERIAL((cell_t)r->crusts_to))
                                   : 0u;
        const unsigned phase = (unsigned)s->step_phase;
        const bool seed_due = ((faces & FACE_FOREIGN) != 0)
                              && ((phase + (unsigned)x * 11u + (unsigned)y * 7u) & (CRUST_SEED_PERIOD - 1u)) == 0u;
        const bool widen_due = ((faces & FACE_CRUST) != 0)
                               && ((phase + (unsigned)x * 5u + (unsigned)y * 33u) & (CRUST_WIDEN_PERIOD - 1u)) == 0u;
        const bool may_crust = seed_due || widen_due;
        if (may_crust
            && (int)(sand_rng_next_at(s, x, y, SAND_RNG_SLOT_REACT_CRUST) & (CRUST_ROLL_MAX - 1))
                   < ((s->crust >= 0) ? s->crust : r->crusts)) {
            REACTION_DOC(crusts_to, "what a settled cell slowly crusts into");
            row[x] = (cell_t)r->crusts_to;
            latch_content_flags(s, row[x]);
            mark_rows(s, x, y, y);
            continue;
        }
        /* Falls through: snow that did not crust this step still chills. */
    stage_chill:
        if (r->chills != 0) {
            if (step_one_cold_cell_or_defer(s, x, y, w, h, r)) {
                found |= FOUND_TEMPERATURE;
            }
            continue;
        }
        /* Gated on `may_have_heat_holder` to avoid unnecessary neighbour
         * scans. */
    stage_warm:
        if (r->warms != 0 && s->may_have_temperature && s->may_have_heat_holder) {
            step_one_warming_cell(s, x, y, w, h, r);
            found |= FOUND_TEMPERATURE;
            continue;
        }
        /* Cheap tests first: field, may_have_liquid, then neighbour scan. */
    stage_soak_dry:
        if ((r->soaks != 0 || r->dries != 0) && step_one_soaking_cell(s, row, x, y, w, h, r)) {
            found |= FOUND_MOISTURE;
            continue;
        }
    stage_fall:
        if (r->falls != 0) {
            found |= FOUND_FALLER;
            /* THE SECOND BIT IS THE SKIP: presence keeps may_have_faller set
             * the way it always did, while a landed or anchored plant reports
             * no mobility, so a board where none can move stops paying for
             * this pass. Sound only because mark_rows() re-arms mobility, and
             * without that a dissolved plant hangs in the air. */
            if (step_one_falling_cell(s, x, y, w, h, r)) {
                found |= FOUND_FALLER_MOVE;
                continue;
            }
        }
    stage_drink:
        if (r->drinks != 0 && s->may_have_liquid) {
            if (step_one_drinking_cell(s, x, y, w, h, r, c)) {
                found |= FOUND_MOISTURE;
            }
        }
    stage_root:
        if (r->roots != 0 && c == (cell_t)r->roots_to && s->may_have_moisture) {
            /* Conduct first, tip growth level constraint. */
            if (step_one_conducting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
            if (step_one_rooting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
            continue;
        }
        /* NEITHER THIS STAGE NOR stage_bud REPORTS FOUND_MOISTURE: both
         * consume moisture, neither is evidence of any, and claiming it let a
         * tree on soil it had drunk dry arm the plant stages off its own
         * existence. Dirt alone carries `soil`, so a dirt cell still holding
         * a drop reports itself at stage_soak_dry regardless. */
    stage_grow:
        if (r->grows != 0 && s->may_have_moisture) {
            step_one_growing_cell(s, x, y, w, h, r);
            continue;
        }
        /* Budding. Same gate as growing, and reached by unlit wood, which
         * falls through every branch above it. */
    stage_sprout:
        if (r->sprouts != 0 && s->may_have_moisture) {
            if (step_one_sprouting_cell(s, x, y, w, h, r)) {
                found |= FOUND_MOISTURE;
            }
        }
        /* Budding, on the same gate. Reached by wood, which falls through
         * every branch above it. */
    stage_bud:
        if (r->buds != 0 && s->may_have_moisture) {
            step_one_budding_cell(s, x, y, w, h, r);
        }
    stage_end:;
    }
    return found;
}

/* BUILT ONCE, NOT PER STEP. Every one of these is a pure function of
 * reactions[], extended_reactions[] and materials[] - all const and
 * flash-resident, unreachable at runtime. reaction_first_stage() alone is a
 * seventeen-field ladder run thirty-two times, and pair_bits is 256 stores,
 * so rebuilding them was a fixed toll on every step of every reacting
 * scene. */
static bool reaction_tables_ready;

static void
build_reaction_tables(void) {
    if (reaction_tables_ready) {
        return;
    }

    uint8_t theirs_bits[MATERIAL_MAX] = {0};
    for (int m = 1; m < MAT_COUNT; m++) {
        const reaction_t* r = &reactions[m];
        if (r->heat_ramp != 0 || (r->heats_to != 0 && (r->heat_chance != 0 || r->melts != 0))) {
            theirs_bits[m] |= PAIR_HEAT_RESPONSIVE;
        }
        if (material_by_id((material_id_t)m)->kind == KIND_LIQUID) {
            if (r->wets != 0) {
                theirs_bits[m] |= PAIR_WETS;
            }
            if (r->flammability == 0 && r->burns == 0) {
                theirs_bits[m] |= PAIR_QUENCHES;
            }
        }
        if (r->flammability != 0) {
            theirs_bits[m] |= PAIR_IGNITABLE;
        }
        if (r->dissolvable != 0) {
            theirs_bits[m] |= PAIR_DISSOLVABLE;
        }
        if (r->conducts != 0) {
            theirs_bits[m] |= PAIR_CONDUCTS;
        }
    }
    /* MATERIAL_EXTENDED_CODES (16), not _COUNT (8): a probe only knows a
     * cell is MAT_EXTENDED, never which of the sixteen codes, so
     * theirs_bits[MAT_EXTENDED] ORs every extended material's bits into one
     * shared slot. Including gunpowder's half is safe since OR only sets
     * bits, never clears one a static already set. */
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        const reaction_t* r = &extended_reactions[k];
        if (r->heat_ramp != 0 || (r->heats_to != 0 && (r->heat_chance != 0 || r->melts != 0))) {
            theirs_bits[MAT_EXTENDED] |= PAIR_HEAT_RESPONSIVE;
        }
        if (r->flammability != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_IGNITABLE;
        }
        if (r->dissolvable != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_DISSOLVABLE;
        }
        if (r->conducts != 0) {
            theirs_bits[MAT_EXTENDED] |= PAIR_CONDUCTS;
        }
    }
    for (int mine = 0; mine < MATERIAL_MAX; mine++) {
        for (int theirs = 0; theirs < MATERIAL_MAX; theirs++) {
            pair_bits[mine][theirs] = theirs_bits[theirs];
        }
    }

    for (int m = 0; m < MAT_COUNT; m++) {
        const bool is_acid_rain_material = (m == MAT_GAS || m == MAT_STEAM);
        material_plan[m].stage = reaction_first_stage(&reactions[m], is_acid_rain_material);
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        extended_plan[k].stage = reaction_first_stage(&extended_reactions[k], false);
    }

    reaction_tables_ready = true;
}

static void
fill_burn_plan(burn_plan_t* p, const sand_t* s, const reaction_t* r, const material_t* mat) {
    const bool lit = r->burn_decay != 0;
    const uint8_t own_rate = lit ? r->burn_decay : mat->decay;
    p->tick_rate = (s->decay >= 0) ? (uint8_t)s->decay : own_rate;
    p->flare = r->flare;
    p->flags = (uint8_t)((lit ? BURN_LIT : 0u)
                         | ((mat->kind != KIND_LIQUID && r->explodes == 0 && mat->density < max_smothering_density)
                                ? BURN_SMOTHERS
                                : 0u)
                         | ((mat->kind == KIND_LIQUID && r->quench_to != 0) ? BURN_LAVA : 0u));
}

/* Indexed exactly as step_one_reacting_row() indexes it, so a suite reading a
 * cell's plan reads the row that cell would really dispatch on. */
const burn_plan_t*
sand_burn_plan_of(cell_t c) {
    return (CELL_MATERIAL(c) == MAT_EXTENDED) ? &extended_plan[CELL_VARIANT(c)] : &material_plan[CELL_MATERIAL(c)];
}

uint8_t
sand_smothering_ceiling(void) {
    return max_smothering_density;
}

static bool reactions_force_full_walk;

void
sand_reactions_force_full_walk(bool on) {
    reactions_force_full_walk = on;
}

/* BLOCK_HAS_MOISTURE is set eagerly wherever a write grants moisture (see
 * mark_block_has_moisture(), sand_priv.h) but never cleared there - nothing
 * at a single write site knows whether it left the block's last moist
 * cell. This is the other half: for every block CURRENTLY flagged, ask
 * whether it still holds one. Cost is bounded by cells in flagged blocks,
 * which is exactly the drying board's own shrinking active region, not
 * REAL_W * REAL_H. */
static void
refresh_moisture_blocks(sand_t* s) {
    for (int by = 0; by < s->block_rows; by++) {
        const int y_lo = by * SAND_BLOCK_H;
        const int y_hi = (y_lo + SAND_BLOCK_H < s->h) ? y_lo + SAND_BLOCK_H : s->h;
        for (int bx = 0; bx < s->block_cols; bx++) {
            uint8_t* const slot = &s->block_state[(size_t)by * (size_t)s->block_cols + (size_t)bx];
            if ((*slot & BLOCK_HAS_MOISTURE) == 0) {
                continue;
            }
            const int x_lo = bx * SAND_BLOCK_W;
            const int x_hi = (x_lo + SAND_BLOCK_W < s->w) ? x_lo + SAND_BLOCK_W : s->w;
            bool still_moist = false;
            for (int y = y_lo; y < y_hi && !still_moist; y++) {
                const uint8_t* const row = s->cells + (size_t)y * (size_t)s->w;
                for (int x = x_lo; x < x_hi; x++) {
                    const cell_t c = row[x];
                    if (CELL_IS_EMPTY(c)) {
                        continue;
                    }
                    const reaction_t* r = reaction_of(c);
                    if (r->dries != 0 && moisture_of(c, r) != 0) {
                        still_moist = true;
                        break;
                    }
                }
            }
            if (!still_moist) {
                *slot &= (uint8_t)~BLOCK_HAS_MOISTURE;
            }
        }
    }
}

/* SOAK-ONLY WALK: every block outside BLOCK_LIQUID_NEAR or BLOCK_HAS_
 * MOISTURE is skipped rather than visited and rejected. Sound only under
 * sand_step_reactions()'s own soak_only gate, which has already ruled out
 * every stage but stage_soak_dry - a proven no-op off both flags. */
static unsigned
step_one_reacting_row_liquid_near(sand_t* s, int y, int w, int h) {
    unsigned found = 0;
    const int by = (int)((unsigned)y / SAND_BLOCK_H);
    for (int bx = 0; bx < s->block_cols; bx++) {
        if ((s->block_state[(size_t)by * (size_t)s->block_cols + (size_t)bx] & (BLOCK_LIQUID_NEAR | BLOCK_HAS_MOISTURE))
            == 0) {
            continue;
        }
        const int x_lo = bx * SAND_BLOCK_W;
        const int x_hi = (x_lo + SAND_BLOCK_W < w) ? x_lo + SAND_BLOCK_W : w;
        found |= step_one_reacting_row(s, y, w, h, x_lo, x_hi);
    }
    return found;
}

/* THE LOCAL-RULE SPLIT: every row, minus its stripe boundary, dispatched
 * through step_one_reacting_row() on whichever core owns it -
 * run_sweep_stripes()'s own shape (sand.c). NO SNAPSHOT COMPARE: unlike a
 * grain's move, a reaction never relocates a cell's row. */
typedef struct {
    sand_t* s;
    int w, h, stripe_h, offset, color, share;
    unsigned* found_out;
} react_phase_ctx_t;

_Static_assert(sizeof(react_phase_ctx_t) <= JOB_CTX_MAX, "react_phase_ctx_t must fit JOB_CTX_MAX");

/* One stripe band's interior rows - the guard row on each side excluded,
 * same shape as run_sweep_stripes()'s own inner_y0/inner_y1 (sand.c). */
static void
react_run_one_stripe(sand_t* s, int w, int h, int band0, int stripe_h, unsigned* found) {
    const int y0 = band0 < 0 ? 0 : band0;
    const int y1 = (band0 + stripe_h > h) ? h : band0 + stripe_h;
    const int inner_y0 = (y0 > 0) ? y0 + 1 : y0;
    const int inner_y1 = (y1 < h) ? y1 - 1 : y1;
    for (int y = inner_y0; y < inner_y1; y++) {
        *found |= step_one_reacting_row(s, y, w, h, 0, w);
    }
}

static void
react_run_stripes(const react_phase_ctx_t* c) {
    const int h = c->h;
    int k = (c->offset == 0) ? 0 : -1;
    int seen = 0;
    unsigned found = 0;

    for (;;) {
        const int band0 = c->offset + k * c->stripe_h;
        if (band0 >= h) {
            break;
        }
        const int stripe_color = ((k % 2) + 2) % 2;
        if (stripe_color == c->color) {
            if ((seen & 1) == c->share) {
                react_run_one_stripe(c->s, c->w, h, band0, c->stripe_h, &found);
            }
            seen++;
        }
        k++;
    }
    *c->found_out = found;
}

static void
react_stripes_worker(void* ctx) {
    react_run_stripes((const react_phase_ctx_t*)ctx);
}

/* One checkerboard phase - every stripe of `color`, minus its guard rows -
 * half on core 1, half here, joined before returning. */
static unsigned
react_run_phase(sand_t* s, int color, int w, int h, int stripe_h, int offset) {
    unsigned found_b = 0;
    react_phase_ctx_t ctx_b = {s, w, h, stripe_h, offset, color, 1, &found_b};
    (void)job_run_core1(react_stripes_worker, &ctx_b, sizeof ctx_b);

    unsigned found_a = 0;
    react_phase_ctx_t ctx_a = {s, w, h, stripe_h, offset, color, 0, &found_a};
    react_run_stripes(&ctx_a);

    (void)job_wait(100);
    return found_a | found_b;
}

#define REACT_GUARD_ROW_MAX (2 * ((GRID_H_MAX + SAND_STRIPE_H_MIN - 1) / SAND_STRIPE_H_MIN))

/* Every guard row this step has - the two rows either side of each stripe
 * boundary, boundary order. Mirrors sweep_guard_row_list() (sand.c) over
 * the same stripe_h/offset; reactions have no sweep direction to reorder
 * for, so unlike that function this list is consumed as-is. */
static int
react_guard_row_list(int h, int stripe_h, int offset, int* out, int max) {
    int k = (offset == 0) ? 0 : -1;
    int n = 0;

    for (;;) {
        const int boundary = offset + (k + 1) * stripe_h;
        if (boundary >= h) {
            break;
        }
        if (boundary > 0) {
            if (n + 1 < max) {
                out[n] = boundary - 1;
                out[n + 1] = boundary;
            }
            n += 2;
        }
        k++;
    }
    return n;
}

static void
reach_confined_ignitions(sand_t* s) {
    const int w = s->w, h = s->h;
    for (uint8_t i = 0; i < react_confined_ignite_defer_count; i++) {
        const int x = react_confined_ignite_defer[i].x;
        const int y = react_confined_ignite_defer[i].y;
        const cell_t c = sand_at(s, x, y);
        if (!CELL_IS_EMPTY(c) && material_of(c)->kind == KIND_GAS && gas_ignite_confined(s, x, y, w, h)) {
            (void)ignite_confined_gas(s, x, y);
        }
    }
    react_confined_ignite_defer_count = 0;
}

static void
reach_lava_bursts(sand_t* s) {
    for (uint8_t i = 0; i < react_lava_burst_defer_count; i++) {
        const int x = react_lava_burst_defer[i].x;
        const int y = react_lava_burst_defer[i].y;
        const cell_t c = sand_at(s, x, y);
        if (CELL_IS_EMPTY(c) || (sand_burn_plan_of(c)->flags & BURN_LAVA) == 0) {
            continue;
        }
        const burning_cell_t cell = {s, reaction_of(c), material_of(c), c, x, y};
        (void)try_lava_burst(&cell);
    }
    react_lava_burst_defer_count = 0;
}

static void
reach_fuse_explosions(sand_t* s) {
    for (uint8_t i = 0; i < react_fuse_explosion_defer_count; i++) {
        const int x = react_fuse_explosion_defer[i].x;
        const int y = react_fuse_explosion_defer[i].y;
        const cell_t c = sand_at(s, x, y);
        const reaction_t* rx = CELL_IS_EMPTY(c) ? NULL : reaction_of(c);
        if (rx == NULL || rx->explodes == 0 || s->fuse_blast_wait != 0) {
            continue;
        }
        s->fuse_blast_wait = (uint8_t)((s->fuse_cooldown >= 0) ? s->fuse_cooldown : SAND_GUNPOWDER_BLAST_COOLDOWN);
        sand_explode(s, x, y, rx->explodes);
    }
    react_fuse_explosion_defer_count = 0;
}

static void
reach_cracks_and_cooloffs(sand_t* s) {
    const int w = s->w, h = s->h;
    for (uint8_t i = 0; i < react_crack_defer_count; i++) {
        const react_crack_defer_t* e = &react_crack_defer[i];
        crack_run(s, e->x, e->y, w, h, (material_id_t)e->from, (material_id_t)e->into);
    }
    react_crack_defer_count = 0;

    for (uint8_t i = 0; i < react_cooloff_defer_count; i++) {
        const react_cooloff_defer_t* e = &react_cooloff_defer[i];
        cool_off_chain(s, e->x, e->y, w, h, e->product, e->chance);
    }
    react_cooloff_defer_count = 0;
}

/* One cell of the re-scan below - conduct_heat() for a still-burning cell,
 * or the whole dissolving/chilling stage for a material that entered the
 * split unable to run either locally. Returns the FOUND_* bits this cell
 * contributes (never FOUND_BURNING - see the header comment on that mask). */
static unsigned
reach_scan_cell(sand_t* s, uint8_t* row, int x, int y, int w, int h) {
    const cell_t c = row[x];
    if (CELL_IS_EMPTY(c)) {
        return 0;
    }
    if (cell_is_burning(c)) {
        (void)conduct_heat(s, x, y, w, h);
        return 0;
    }
    const reaction_t* r = reaction_of(c);
    if (r->dissolves != 0) {
        if (CELL_MATERIAL(c) == MAT_ACID) {
            acid_bubble(s, x, y);
        }
        step_one_dissolver_cell(s, row, x, y, w, h, r);
        return FOUND_DISSOLVER;
    }
    if (r->chills != 0 && step_one_cold_cell(s, x, y, w, h, r)) {
        return FOUND_TEMPERATURE;
    }
    return 0;
}

static unsigned
reach_rescan(sand_t* s) {
    const int w = s->w, h = s->h;
    unsigned found = 0;
    for (int y = 0; y < h; y++) {
        uint8_t* const row = s->cells + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            found |= reach_scan_cell(s, row, x, y, w, h);
        }
    }
    return found;
}

/* THE SERIAL REACH PASS: every `_or_defer` gate's trigger, resolved once,
 * single core, after both phases and the guard rows join. Explosions run
 * first since they can consume a cell outright, before the re-scan sees it. */
static unsigned
sand_step_reaction_reach(sand_t* s) {
    reach_confined_ignitions(s);
    reach_lava_bursts(s);
    reach_fuse_explosions(s);
    reach_cracks_and_cooloffs(s);
    return reach_rescan(s);
}

/* Whether this step's reaction pass may split - the sweep's own gate, plus
 * growers excluded (their own reach was never audited for this) and the
 * narrower soak-only walk left alone; see sand_step_reactions(). */
static bool
reactions_may_split(const sand_t* s, bool soak_only) {
    return sand_two_core_step_enabled() && !soak_only && sand_stripe_count(s) >= SAND_STRIPE_SPLIT_MIN_COUNT
           && (s->may_have_materials & grower_mask()) == 0;
}

/* Every row of the reaction pass, split or not - see reactions_may_split()
 * for the gate and the two block comments above for what each half does. */
static unsigned
run_reaction_rows(sand_t* s, bool soak_only) {
    const int w = s->w;
    const int h = s->h;
    unsigned found = 0;

    if (!reactions_may_split(s, soak_only)) {
        for (int y = 0; y < h; y++) {
            found |=
                soak_only ? step_one_reacting_row_liquid_near(s, y, w, h) : step_one_reacting_row(s, y, w, h, 0, w);
        }
        return found;
    }

    const int stripe_h = sand_stripe_height(h);
    const int offset = sand_stripe_offset(s);
    s->rng_hashed = true;
    found |= react_run_phase(s, 0, w, h, stripe_h, offset);
    found |= react_run_phase(s, 1, w, h, stripe_h, offset);

    int guard_rows[REACT_GUARD_ROW_MAX];
    const int guard_count = react_guard_row_list(h, stripe_h, offset, guard_rows, REACT_GUARD_ROW_MAX);
    for (int gi = 0; gi < guard_count; gi++) {
        found |= step_one_reacting_row(s, guard_rows[gi], w, h, 0, w);
    }
    s->rng_hashed = false;

    return found | sand_step_reaction_reach(s);
}

void
sand_step_reactions(sand_t* s) {
    sand_reactions_last_was_soak_only = false;

    if (s->fuse_blast_wait != 0) {
        s->fuse_blast_wait--;
    }
    /* Dissolving, not fire. Heat, condensation independent. */
    /* A LIQUID WITH SOMEWHERE TO GO is the missing term. Without it this
     * returned on a board whose water had not landed yet, and soaking and
     * drinking - the only two ways new moisture is made - never ran again.
     * Measured on HEAD: water dropped eight rows onto dry dirt stayed dry for
     * 400 steps. */
    if (!s->may_have_burning && !s->may_have_dissolver && !s->may_have_temperature && !s->may_have_moisture
        && !(s->may_have_liquid && (s->may_have_materials & wettable_mask()) != 0)
        && !(s->may_have_faller && s->faller_may_move) && !s->may_have_condenser) {
        return;
    }

    build_reaction_tables();

    /* Materials -> bits, once, here: this is the first point in a step where
     * the table is known built, and the passes below read the result per
     * cell. */
    present_pair_bits = 0;
    max_smothering_density = 0;
    acid_rain_possible =
        (s->may_have_materials & (1u << MAT_STEAM)) != 0 && (s->may_have_materials & (1u << MAT_GAS)) != 0;
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if ((s->may_have_materials & (1u << m)) == 0) {
            continue;
        }
        present_pair_bits |= pair_theirs_bits((uint8_t)m);

        /* MAT_EXTENDED is one bit over several materials, so it contributes
         * the densest of them - the conservative direction for a skip. */
        if (m == MAT_EXTENDED) {
            for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
                const material_t* em = material_of(MATX(k));
                if (em->kind != KIND_LIQUID && em->density > max_smothering_density) {
                    max_smothering_density = em->density;
                }
            }
            continue;
        }
        const material_t* mm = material_of(CELL_MAKE((uint8_t)m, 0));
        if (mm->kind != KIND_LIQUID && mm->density > max_smothering_density) {
            max_smothering_density = mm->density;
        }
    }

    /* Built for every row, not only the present ones: a reaction can create a
     * material this pass, and the plan it then dispatches on has to be there. */
    for (int m = 0; m < MATERIAL_MAX; m++) {
        fill_burn_plan(&material_plan[m], s, &reactions[m], material_of(CELL_MAKE((uint8_t)m, 0)));
    }
    for (int k = 0; k < MATERIAL_EXTENDED_CODES; k++) {
        fill_burn_plan(&extended_plan[k], s, &extended_reactions[k], material_of(CELL_MAKE(MAT_EXTENDED, (uint8_t)k)));
    }

    /* SOAK-ONLY: every other stage's presence flag reads quiet and no
     * drinker's find_water() reaches past a block, per drinker_mask() -
     * only stage_soak_dry is left, block-local via liquid_near(). may_have_
     * moisture matters only alongside grower_mask(): grow/sprout/bud/root-
     * weld are its only readers, unreachable with none present. */
    const bool soak_only = !reactions_force_full_walk && !s->may_have_burning && !s->may_have_dissolver
                           && !s->may_have_temperature
                           && !(s->may_have_moisture && (s->may_have_materials & grower_mask()) != 0)
                           && !(s->may_have_faller && s->faller_may_move) && !s->may_have_condenser
                           && (s->may_have_liquid || s->may_have_moisture)
                           && (s->may_have_materials & drinker_mask()) == 0 && s->block_state != NULL;
    sand_reactions_last_was_soak_only = soak_only;
    if (soak_only) {
        refresh_moisture_blocks(s);
    }

    /* CLEARED HERE so a bit latch_content_flags() ORs in mid-pass survives the
     * write-back below; assigning the walk's census there dropped cells this
     * pass CREATED at its own coordinates. The other five may_have_* bools
     * gate stage_warm and the plants per cell, so they clear at the end; the
     * fall pair gates nothing in the pass and joins the mask here.
     *
     * SOAK-ONLY skips the clear: an unvisited block proves nothing gone. */
    if (!soak_only) {
        s->may_have_materials = 0;
    }
    seen_materials = 0;

    /* CLEARED HERE, not with the five below, for the same reason the mask
     * above is: this pass dissolves and burns ground, and each of those marks
     * a row. Clearing at the end throws that arming away and leaves a plant
     * over the hole this same pass opened under it. Presence joins it so a
     * plant BUDDED mid-pass, into a row already walked, is not cleared away
     * either.
     *
     * SOAK-ONLY SKIPS THIS CLEAR TOO, same reasoning as the materials mask
     * above. */
    if (!soak_only) {
        s->may_have_faller = false;
        s->faller_may_move = false;
    }

    const unsigned found = run_reaction_rows(s, soak_only);

    if (!(found & FOUND_BURNING)) {
        s->may_have_burning = false;
    }
    if (!(found & FOUND_DISSOLVER)) {
        s->may_have_dissolver = false;
    }
    if (!(found & FOUND_TEMPERATURE)) {
        s->may_have_temperature = false;
    }
    /* ARMS AS WELL AS CLEARS, unlike the three above. Cleared-only, a pour
     * still in mid-air cleared it and the soil it landed on could never
     * re-arm the growth stages. Losing a bit latched mid-pass is safe here,
     * unlike for may_have_materials: whatever else latches it is a liquid,
     * which the early return above keeps the pass alive for. */
    s->may_have_moisture = (found & FOUND_MOISTURE) != 0;
    if ((found & FOUND_FALLER) != 0) {
        s->may_have_faller = true;
    }
    if ((found & FOUND_FALLER_MOVE) != 0) {
        s->faller_may_move = true;
    }
    if (!(found & FOUND_CONDENSING)) {
        s->may_have_condenser = false;
    }
    /* Same shape as the flags above: OR, so a material created mid-pass by
     * place_cell() keeps the bit it just latched. */
    s->may_have_materials |= seen_materials;

    /* may_have_heat_holder NOT cleared; clearing at end is wrong. */

    /* may_have_heat_holder: cell with heat_ramp can be created during this
     * pass. */

    /* Clearing wipes flag; convection dead. */

    /* Measured: arm-only version byte-identical to HEAD on eight scenes. */

    /* Nobody pays per-step upkeep once armed. */
}
