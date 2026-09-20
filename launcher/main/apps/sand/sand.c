/*
 * sand - a falling-sand cellular automaton.
 *
 * The whole simulation is one rule applied to every grain: try to move the way
 * gravity points; failing that, try the two directions either side of it. Piles
 * with a natural angle of repose, heaps that collapse when undermined and sand
 * that pours through a gap all fall out of those three attempts. Nothing here
 * models them explicitly.
 *
 * The one subtlety is sweep order - see the comment on sand_step().
 *
 * A liquid's DOWN-AND-SLIDE movement is here too, in move_liquid_grain() -
 * called from the same sweep, because it obeys the same gravity-ward
 * guarantee every other move in this file does. Everything else about a
 * liquid - the two things that are NOT gravity-ward - lives in
 * sand_liquid.c. See sand_priv.h for why they need to share a few small
 * helpers, and sand_step_liquids() for where the two meet.
 */

#include "sand_priv.h"

#include <stdlib.h>
#include <string.h>

#ifdef DEVICE_BUILD
#include "esp_timer.h"
#endif

#include "build_variant.h"
#include "sand_limits.h"
#include "sand_liquid_move.h"
#include "util/fixed.h"
#include "util/intmath.h"

/* See sand_priv.h. Defined here, not sand_liquid.c: move_liquid_grain()
 * (sand_liquid_move.h) is called only from this file's own sweep. */
unsigned sand_liquid_sweep_moves;

#ifdef ESP_PLATFORM
static bool two_core_step_on = true;
#else
static bool two_core_step_on = false;
#endif

void
sand_set_two_core_step(bool on) {
    two_core_step_on = on;
}

bool
sand_two_core_step_enabled(void) {
    return two_core_step_on;
}

/* tan(22.5 deg) is the boundary between "straight down" and "diagonal"; its
 * reciprocal, 2.4142, is approximated as 29/12 to keep this in integers.
 * The largest operand is a raw accelerometer reading, so 32767 * 29 stays well
 * inside 32 bits. */
#define AXIS_NUM         29
#define AXIS_DEN         12

/* How long the poured shade lingers on one part of the band before
 * drifting on: 64 steps, about two seconds, so a single brushful is one
 * shade and two separate pours are two. */
#define POUR_BAND_SHIFT  6

/* Jumps shades instead of stepping one at a time. Walking one shade apart
 * results in a gap of about twenty luminance points. Five is coprime with 12,
 * 16, 8, and 3, so all shades are visited but not in order. Stride must be
 * coprime with span to visit all shades. */
#define POUR_BAND_STRIDE 5u

/* `band` centers this pour's shade, computed once by the caller for all
 * cells. Modulo uses real division due to MATERIAL_SHADE_SPAN's runtime
 * ternary. Computed per cell, it costs about 11% of the spawn path; hoisting
 * reduces this to one calculation per brushful. */
static cell_t
random_cell(sand_t* s, material_id_t material, int band) {
    /* A liquid's variant is an amount, not a shade, so a fresh cell is a full
     * one. Giving it a random level would be pouring random quantities. */
    if (material_by_id(material)->kind == KIND_LIQUID) {
        return CELL_MAKE(material, MASS_MAX);
    }
    /* A transient material's variant is life remaining, not a shade either -
     * see material.h's top comment and the `decay` field it documents.
     * Fresh gas starts at full life so it fades from vivid to gone, rather
     * than spawning already partway decayed. */
    if (material_by_id(material)->decay != 0) {
        return CELL_MAKE(material, MATERIAL_VARIANTS - 1);
    }
    /* A heat-ramping material's variant is a TEMPERATURE, not a shade. A
     * fresh cell is at ROOM temperature, not the bottom of its range
     * (frosted). Random would give a pane already half melted, and
     * MATERIAL_VARIANTS - 1 would melt on the next step. */
    if (reactions[material].heat_ramp != 0) {
        return CELL_MAKE(material, SAND_AMBIENT_HEAT);
    }
    /* A material that burns only while lit has HOW MUCH IS LEFT TO BURN in
     * its variant, and a fresh one is not on fire. A random shade would
     * hand the player a log that is already half burnt - and, at variant
     * 0 being the only unlit value, mostly one that is already alight. */
    if (reactions[material].burn_decay != 0) {
        return CELL_MAKE(material, 0);
    }
    /* Dry cells have no moisture, picking a SOIL_DRY_TONES shade instead.
     * Fresh soil is always dry to avoid giving players fertile ground. `band`
     * is pre-folded into the dry range, using +/-1 jitter for varied tones. */
    if (reactions[material].dries != 0) {
        const reaction_t* r = &reactions[material];
        int tone = band + (int)rng_below(&s->rng, 3) - 1;
        if (tone < 0) {
            tone = 0;
        } else if (tone >= r->tones) {
            tone = r->tones - 1;
        }
        /* soil_cell(), the table-driven form of CELL_SOIL() - see
         * material.h. Byte-identical for dirt (r->tones ==
         * SOIL_DRY_TONES), and the form that keeps working if a second
         * material ever sets `dries != 0`. */
        return soil_cell(CELL_MAKE(material, 0), (uint8_t)tone, 0, r);
    }
    /* Sand retains top four shades; painted dune avoids window grains.
     * Centred on drifted band. Brushfuls vary slightly, showing layers.
     * Jitter (+/-1 shade) preserves grain texture, not flatness. Single draw
     * remains. */
    const int span = MATERIAL_SHADE_SPAN(material);
    int shade = band + (int)rng_below(&s->rng, 3) - 1;
    if (shade < 0) {
        shade = 0;
    } else if (shade >= span) {
        shade = span - 1;
    }
    return CELL_MAKE(material, (uint8_t)shade);
}

/* Mirrors random_cell() above but keyed off reaction_of()'s tones, not
 * MATERIAL_SHADE_SPAN() - GUNPOWDER's variant space is narrower and
 * comes from GUNPOWDER_REACTION, not the material table. */
static cell_t
random_gunpowder(sand_t* s, int band) {
    const reaction_t* r = reaction_of(GUNPOWDER_BASE);
    int tone = band + (int)rng_below(&s->rng, 3) - 1;
    if (tone < 0) {
        tone = 0;
    } else if (tone >= r->tones) {
        tone = r->tones - 1;
    }
    return GUNPOWDER_CELL((uint8_t)tone);
}

/* Grid access */

void
sand_init(sand_t* s, uint8_t* cells, int w, int h, uint32_t seed) {
    s->cells = cells;
    s->w = w;
    s->h = h;
    rng_seed(&s->rng, seed);
    s->rng_seed_base = seed;
    s->rng_hashed = false;
    s->pour_phase = 0;
    s->step_phase = 0;
    s->sweep_flip = false;
    s->liquid_flip = false;
    s->gas_flip = false;
    s->fuse_blast_wait = 0;
    s->fuse_cooldown = -1; /* see sand_set_fuse_cooldown() */
    /* Resets every content flag, so a reused sand_t cannot carry a stale one
     * into a fresh board and wake reactions it shouldn't. */
    clear_content_flags(s);
    s->dirty_rows = NULL;
    s->dirty_x0 = NULL;
    s->dirty_x1 = NULL;
    s->block_state = NULL;
    s->step_stamps = NULL;
    s->stamps_live = NULL;
    s->stamp_side = 0;
    s->stamped = false;
    s->lane_scratch = NULL;
    s->impulse_buf = NULL;
    s->impulse_max = 0;
    s->impulse_count = 0;
    /* Cleared again at the top of every sand_step(), but a caller driving one
     * pass at a time never reaches that line and would read its own frame. */
    s->explosions_this_step = 0;
    s->confined_blasts_this_step = 0;
#ifdef DEVICE_BUILD
    s->impulse_cap_hits = 0;
    s->split_lane_aborts = 0;
#endif
    s->splash_chance = SAND_SPLASH_CHANCE_START;
    s->splash_radius_water = SAND_SPLASH_RADIUS_WATER;
    s->heat_flaw_seq = 0;
    s->heat_flaw_is_flawed = false;
    /* Computed unconditionally: main sweep always walks block-columns (see
     * step_one_row()), requiring real grid-derived block_cols/block_rows,
     * never zero. */
    s->block_cols = (w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    s->block_rows = (h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;
    s->last_load_dx = 0;
    s->last_load_dy = 0;
    s->last_step_dx = 0;
    s->last_step_dy = 0;
    s->scatter = 0;
    s->decay = 0;
    s->evaporates = 0; /* see sand_set_evaporates() */
    s->soak_convert = SAND_SOAK_CONVERT_PER_MATERIAL;
    s->soak = 0; /* nothing soaks unless asked - see
                             * sand_set_soak() */
    s->mobility = 255;
    s->may_have_viscous_liquid = false;                            /* full speed by default - see sand_set_mobility() */
    s->gas_walk = true;                                            /* random walk, 23% cheaper - see sand_set_gas_walk()
                                * for the deterministic exhaustive mover */
    s->flammability = SAND_FLAMMABILITY_PER_MATERIAL;              /* see sand_set_flammability() */
    s->conduction = SAND_CONDUCTION_PER_MATERIAL;                  /* see sand_set_conduction() */
    s->boils = SAND_BOILS_PER_MATERIAL;                            /* see sand_set_boils() */
    s->condenses = SAND_CONDENSES_PER_MATERIAL;                    /* see sand_set_condenses() */
    s->lava_cooloff = SAND_LAVA_COOLOFF_DEFAULT;                   /* see sand_set_lava_cooloff() */
    s->lava_burst = SAND_LAVA_BURST_DEFAULT;                       /* see sand_set_lava_burst() */
    s->crust = -1;                                                 /* see sand_set_crust() */
    s->acid_rain = SAND_ACID_RAIN_DEFAULT;                         /* see sand_set_acid_rain() */
    s->acid_dilute_mass_bias = SAND_ACID_DILUTE_MASS_BIAS_DEFAULT; /* see
                                          * sand_set_acid_dilute_mass_bias() */
    /* The array itself need not be touched - every reader below goes
     * through emitter_count, so an entry past it is simply never looked
     * at, the same way sand_spawn_cell()'s clipped cells are never looked
     * at rather than being separately zeroed. */
    s->emitter_count = 0;
    sand_clear(s);
}

/* wake_blocks_range()/wake_block_and_neighbors() and mark_rows()/mark_move()
 * are shared with sand_liquid.c and live in sand_priv.h.
 * BLOCK_SETTLED_NEAREST/OTHER/ACTIVE (block_state) also live there. */

void
sand_enable_sleeping(sand_t* s, uint8_t* blocks) {
    s->block_state = blocks;
    if (blocks != NULL) {
        /* Nothing is known about the grid yet, so nothing may be assumed
         * settled. */
        memset(blocks, 0, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    s->last_load_dx = 0;
    s->last_load_dy = 0;
}

size_t
sand_step_stamp_bytes(int w, int h) {
    return (size_t)h * sand_stamp_stride(w);
}

void
sand_enable_step_stamps(sand_t* s, uint8_t* bits) {
    s->step_stamps = bits;
    if (bits != NULL) {
        memset(bits, 0, sand_step_stamp_bytes(s->w, s->h));
    }
}

size_t
sand_lane_scratch_bytes(int w, int h) {
    const size_t rows = (size_t)h;
    const size_t blocks =
        (size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H);

    return SAND_LANE_COUNT * (2 * rows * sizeof(uint16_t) + blocks + rows + SAND_LANE_DEFER_BYTES);
}

void
sand_enable_lane_scratch(sand_t* s, void* scratch) {
    s->lane_scratch = scratch;
}

/* One pair for every board: the passes that use it never overlap, and a
 * lane carries a whole sand_t, which is far too much to hand a job context
 * or to leave on a frame core 1 can outlive. */
static sand_lane_t sand_lane_pair[SAND_LANE_COUNT];

sand_lane_t*
sand_lanes(sand_t* s) {
    if (s->lane_scratch == NULL) {
        return NULL;
    }
    const size_t rows = (size_t)s->h;
    const size_t blocks = (size_t)s->block_cols * (size_t)s->block_rows;
    uint16_t* const spans = s->lane_scratch;
    uint8_t* const bytes = (uint8_t*)(spans + (size_t)(2 * SAND_LANE_COUNT) * rows);

    uint8_t* const defer = bytes + (size_t)SAND_LANE_COUNT * (blocks + rows);

    for (int i = 0; i < SAND_LANE_COUNT; i++) {
        sand_lane_pair[i].x0 = spans + (size_t)(2 * i) * rows;
        sand_lane_pair[i].x1 = sand_lane_pair[i].x0 + rows;
        sand_lane_pair[i].blocks = bytes + (size_t)i * (blocks + rows);
        sand_lane_pair[i].dirty = sand_lane_pair[i].blocks + blocks;
        sand_lane_pair[i].defer = defer + (size_t)i * SAND_LANE_DEFER_BYTES;
    }
    return sand_lane_pair;
}

void
sand_lane_prepare(sand_lane_t* lane, const sand_t* s) {
    lane->local = *s;
    lane->local.rng_hashed = true;
    if (s->block_state != NULL) {
        lane->local.block_state = lane->blocks;
        memcpy(lane->blocks, s->block_state, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    if (s->dirty_rows != NULL) {
        lane->local.dirty_rows = lane->dirty;
        memcpy(lane->dirty, s->dirty_rows, (size_t)s->h);
    }
    if (s->dirty_x0 != NULL && s->dirty_x1 != NULL) {
        lane->local.dirty_x0 = lane->x0;
        lane->local.dirty_x1 = lane->x1;
        memcpy(lane->x0, s->dirty_x0, sizeof *lane->x0 * (size_t)s->h);
        memcpy(lane->x1, s->dirty_x1, sizeof *lane->x1 * (size_t)s->h);
    }
}

static void
merge_lane_content_flags(sand_t* s, const sand_t* lane) {
    s->may_have_liquid |= lane->may_have_liquid;
    s->may_have_gas |= lane->may_have_gas;
    s->may_have_burning |= lane->may_have_burning;
    s->may_have_dissolver |= lane->may_have_dissolver;
    s->may_have_temperature |= lane->may_have_temperature;
    s->may_have_moisture |= lane->may_have_moisture;
    s->may_have_faller |= lane->may_have_faller;
    s->may_have_heat_holder |= lane->may_have_heat_holder;
    s->may_have_condenser |= lane->may_have_condenser;
    s->may_have_viscous_liquid |= lane->may_have_viscous_liquid;
    s->may_have_materials |= lane->may_have_materials;
    s->faller_may_move |= lane->faller_may_move;
    s->stamped |= lane->stamped;
}

/* A settled bit survives only where both agree; everything else a lane can
 * write it only sets. */
static void
merge_lane_blocks(sand_t* s, const uint8_t* lane_blocks) {
    for (int i = 0; i < s->block_cols * s->block_rows; i++) {
        const uint8_t local = lane_blocks[i];
        s->block_state[i] &= (uint8_t)(local | ~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER));
        s->block_state[i] |= local & (BLOCK_ACTIVE | BLOCK_HAS_LIQUID | BLOCK_HAS_MOISTURE);
    }
}

static void
merge_lane_spans(sand_t* s, const uint16_t* x0, const uint16_t* x1) {
    for (int y = 0; y < s->h; y++) {
        if (x0[y] < s->dirty_x0[y]) {
            s->dirty_x0[y] = x0[y];
        }
        if (x1[y] > s->dirty_x1[y]) {
            s->dirty_x1[y] = x1[y];
        }
    }
}

void
sand_lane_merge(sand_t* s, const sand_lane_t* lane) {
    if (s->block_state != NULL) {
        merge_lane_blocks(s, lane->blocks);
    }
    if (s->dirty_rows != NULL) {
        for (int y = 0; y < s->h; y++) {
            s->dirty_rows[y] |= lane->dirty[y];
        }
    }
    if (s->dirty_x0 != NULL && s->dirty_x1 != NULL) {
        merge_lane_spans(s, lane->x0, lane->x1);
    }
    merge_lane_content_flags(s, &lane->local);
}

bool
sand_block_settled(const sand_t* s, int bx, int by) {
    if (s->block_state == NULL) {
        return false;
    }
    return (s->block_state[by * s->block_cols + bx] & (BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER)) != 0;
}

void
sand_track_dirty_rows(sand_t* s, uint8_t* rows) {
    s->dirty_rows = rows;
    if (rows != NULL) {
        /* Nothing is known about what is already on screen, so assume all of
         * it needs redrawing once. */
        memset(rows, 1, (size_t)s->h);
    }
}

/* x0[y] > x1[y] is the sentinel for "no column recorded" - a row marked
 * dirty with a sentinel span falls back to a full-width repaint, exactly
 * what dirty_rows alone drew before column tracking existed. */
static void
reset_dirty_cols(sand_t* s) {
    if (s->dirty_x0 == NULL || s->dirty_x1 == NULL) {
        return;
    }
    for (int y = 0; y < s->h; y++) {
        s->dirty_x0[y] = (uint16_t)s->w;
        s->dirty_x1[y] = 0;
    }
}

void
sand_track_dirty_cols(sand_t* s, uint16_t* x0, uint16_t* x1) {
    s->dirty_x0 = x0;
    s->dirty_x1 = x1;
    reset_dirty_cols(s);
}

void
sand_clear(sand_t* s) {
    memset(s->cells, SAND_EMPTY, (size_t)s->w * (size_t)s->h);
    /* Explicit full-width spans, not the sentinel: a later narrow mark in the
     * same frame unions into a sentinel and would shrink the wipe. */
    for (int y = 0; y < s->h; y++) {
        mark_row_span(s, y, 0, s->w - 1);
    }
    if (s->block_state != NULL) {
        memset(s->block_state, 0, (size_t)s->block_cols * (size_t)s->block_rows);
    }
    /* Any in-flight entry names a cell just wiped, so its stored `cell` byte
     * no longer matches. Dropping them here avoids paying for it later: no
     * stale index survives. */
    s->impulse_count = 0;
}

cell_t
sand_at(const sand_t* s, int x, int y) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        /* Outside the grid reads as STONE, which makes the four walls solid
         * without a single bounds check in the movement code - and, being the
         * densest thing there is, too solid for anything heavy to displace its
         * way through the floor. */
        return CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT);
    }
    return s->cells[y * s->w + x];
}

void
sand_set(sand_t* s, int x, int y, cell_t cell) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return;
    }
    s->cells[y * s->w + x] = cell;
    latch_content_flags(s, cell);
    mark_move(s, x, y, x, y);
}

int
sand_count(const sand_t* s) {
    int n = 0;
    const int total = s->w * s->h;
    for (int i = 0; i < total; i++) {
        n += (s->cells[i] != SAND_EMPTY) ? 1 : 0;
    }
    return n;
}

/* Attempt to place `material` at (x, y). Returns whether it did - off the
 * grid or already occupied is not an error, just nothing to do. */
static bool
try_spawn_one(sand_t* s, int x, int y, cell_t spec, int band) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return false;
    }
    if (s->cells[y * s->w + x] != SAND_EMPTY) {
        return false; /* never overwrite, so the count cannot drift */
    }
    /* Latched from the finished cell due to variant-dependent flags using
     * sand_set(). Statics written as given, with low nibble as identity.
     * Gunpowder uses random_gunpowder() due to its identity-adjacent bits. */
    const cell_t cell = cell_is_extended(spec)    ? spec
                        : cell_is_gunpowder(spec) ? random_gunpowder(s, band)
                                                  : random_cell(s, (material_id_t)CELL_MATERIAL(spec), band);
    s->cells[y * s->w + x] = cell;
    latch_content_flags(s, cell);
    mark_move(s, x, y, x, y);
    return true;
}

int
sand_spawn(sand_t* s, int cx, int cy, int radius, material_id_t material) {
    return sand_spawn_cell(s, cx, cy, radius, CELL_MAKE(material, 0));
}

int
sand_spawn_cell(sand_t* s, int cx, int cy, int radius, cell_t spec) {
    return sand_spawn_cell_share(s, cx, cy, radius, spec, SAND_SPAWN_SHARE_FULL);
}

int
sand_spawn_cell_share(sand_t* s, int cx, int cy, int radius, cell_t spec, int share_pct) {
    int filled = 0;
    const int r2 = radius * radius;
    /* Once for the whole brushful - see random_cell(). material_shade_span_
     * cell(), not the plain id-only macro, because `spec` may be gunpowder
     * (whose span is 3, read off its own reaction row) rather than a
     * material_id_t CELL_MATERIAL() could safely extract a span for. */
    const int span = material_shade_span_cell(spec);
    const int band = (int)(((s->pour_phase >> POUR_BAND_SHIFT) * POUR_BAND_STRIDE) % (unsigned)span);
    /* No roll at all at a full share: a solid pour must draw exactly the
     * random numbers a share-less one draws, or every scene's outcome
     * shifts with it. */
    const bool thinned = share_pct < SAND_SPAWN_SHARE_FULL;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const int x = cx + dx;
            const int y = cy + dy;
            /* Through sand_rng_next_at()'s wrapper rather than the plain
             * stream: a pour reached inside a checkerboard-parallel pass
             * would otherwise draw from state two cores share. */
            if (thinned && sand_rng_below_at(s, x, y, SAND_RNG_SLOT_SPAWN_SHARE, SAND_SPAWN_SHARE_FULL) >= share_pct) {
                continue;
            }
            if (try_spawn_one(s, x, y, spec, band)) {
                filled++;
            }
        }
    }
    return filled;
}

int
sand_erase(sand_t* s, int cx, int cy, int radius) {
    int removed = 0;
    const int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const int x = cx + dx;
            const int y = cy + dy;
            if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
                continue;
            }
            if (s->cells[y * s->w + x] == SAND_EMPTY) {
                continue; /* already empty, so nothing changed here */
            }
            s->cells[y * s->w + x] = SAND_EMPTY;
            mark_move(s, x, y, x, y);
            removed++;
        }
    }

    /* Also switches off any emitter in the same disc - see this function's
     * own comment in sand.h for why. Not folded into `removed`: that count
     * means cells changed, and an emitter is not a cell. */
    sand_remove_emitters(s, cx, cy, radius);

    return removed;
}

/*
 * Emitters - see the `emitters` field of sand_t and the EMITTERS section of
 * sand.h for the design. What is here is just list management; the actual
 * per-step write lives in emit_from_emitters() below, next to sand_step().
 */

bool
sand_add_emitter(sand_t* s, int x, int y, cell_t cell) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) {
        return false;
    }

    /* Retune in place if one is already here, rather than adding a second
     * - see sand_add_emitter()'s own comment in sand.h. */
    for (int i = 0; i < s->emitter_count; i++) {
        if (s->emitters[i].x == x && s->emitters[i].y == y) {
            s->emitters[i].cell = cell;
            return true;
        }
    }

    if (s->emitter_count >= SAND_MAX_EMITTERS) {
        return false;
    }

    s->emitters[s->emitter_count].x = (int16_t)x;
    s->emitters[s->emitter_count].y = (int16_t)y;
    s->emitters[s->emitter_count].cell = cell;
    s->emitter_count++;
    return true;
}

int
sand_remove_emitters(sand_t* s, int cx, int cy, int radius) {
    const int r2 = radius * radius;
    int removed = 0;
    int kept = 0;

    /* Compact in place: every emitter that survives the disc test is
     * copied down to the next free slot, so the surviving emitters end up
     * contiguous at the front with no gap for a later sand_emitter_at() to
     * trip over. */
    for (int i = 0; i < s->emitter_count; i++) {
        const int dx = s->emitters[i].x - cx;
        const int dy = s->emitters[i].y - cy;
        if (dx * dx + dy * dy <= r2) {
            removed++;
            continue;
        }
        if (kept != i) {
            s->emitters[kept] = s->emitters[i];
        }
        kept++;
    }
    s->emitter_count = kept;
    return removed;
}

int
sand_emitter_count(const sand_t* s) {
    return s->emitter_count;
}

bool
sand_emitter_at(const sand_t* s, int i, int* x, int* y, cell_t* cell) {
    if (i < 0 || i >= s->emitter_count) {
        return false;
    }
    *x = s->emitters[i].x;
    *y = s->emitters[i].y;
    *cell = s->emitters[i].cell;
    return true;
}

/* Via sand_spawn_cell(), never a raw write: `emitters[i].cell` is a
 * placeholder (CELL_MAKE(material, 0)), and for a KIND_LIQUID variant 0
 * is zero mass - a raw write once placed a lava emitter with nothing in
 * it, never rendering or flowing. Radius 0, not a disc, so the source
 * cannot bury itself; sand_spawn_cell() also wakes the block. */
static void
emit_from_emitters(sand_t* s) {
    for (int i = 0; i < s->emitter_count; i++) {
        sand_spawn_cell(s, s->emitters[i].x, s->emitters[i].y, 0, s->emitters[i].cell);
    }
}

/* Movement */

/* The sign/magnitude split both gravity_direction functions below start
 * with. Returns false for a zero vector, in which case the direction is
 * undefined and the caller must stop rather than divide by it. */
static bool
gravity_axes(int gx, int gy, int* ax, int* ay, int* sx, int* sy) {
    *ax = im_abs(gx);
    *ay = im_abs(gy);

    if (*ax == 0 && *ay == 0) {
        return false;
    }

    *sx = im_sign(gx);
    *sy = im_sign(gy);
    return true;
}

void
sand_gravity_direction(int gx, int gy, int* dx, int* dy) {
    int ax, ay, sx, sy;
    if (!gravity_axes(gx, gy, &ax, &ay, &sx, &sy)) {
        *dx = 0;
        *dy = 0;
        return;
    }

    if (ay * AXIS_DEN > ax * AXIS_NUM) {
        *dx = 0; /* within 22.5 deg of vertical */
        *dy = sy;
    } else if (ax * AXIS_DEN > ay * AXIS_NUM) {
        *dx = sx; /* within 22.5 deg of horizontal */
        *dy = 0;
    } else {
        *dx = sx; /* the diagonal octant */
        *dy = sy;
    }
}

/* Counts grains above, capped. Does NOT use sand_at() as it reports
 * out-of-bounds as occupied, making walls solid. Here, off-grid is open sky,
 * not occupied. */
int
sand_load_above(const sand_t* s, int x, int y, int dx, int dy) {
    int n = 0;
    int cx = x - dx;
    int cy = y - dy;

    while (n < SAND_LOAD_CAP) {
        if (cx < 0 || cx >= s->w || cy < 0 || cy >= s->h) {
            break;
        }
        if (s->cells[cy * s->w + cx] == SAND_EMPTY) {
            break;
        }
        n++;
        cx -= dx;
        cy -= dy;
    }
    return n;
}

/* True angle's diagonal lean (0-256). `r` is ratio of smaller to larger
 * component (0-256), 0 on axis, 256 at 45 degrees. Angle position: Rajan's
 * approximation, atan(r/256) / 45deg, 0.3477 * 256 = 89. Accurate within a
 * degree. */
static int
diagonal_weight(int r) {
    return r + ((89 * r * (256 - r)) >> 16);
}

void
sand_gravity_direction_dithered(sand_t* s, int gx, int gy, int* dx, int* dy) {
    int ax, ay, sx, sy;
    if (!gravity_axes(gx, gy, &ax, &ay, &sx, &sy)) {
        *dx = 0;
        *dy = 0;
        return;
    }

    const int lo = ax < ay ? ax : ay;
    const int hi = ax < ay ? ay : ax;

    /* hi is non-zero here, since not both components are zero. */
    const int r = (int)(((int64_t)lo * 256) / hi);

    if (rng_chance(&s->rng, diagonal_weight(r))) {
        *dx = sx; /* the diagonal between the two axes */
        *dy = sy;
    } else if (ax > ay) {
        *dx = sx; /* the dominant axis */
        *dy = 0;
    } else {
        *dx = 0;
        *dy = sy;
    }
}

void
sand_set_scatter(sand_t* s, int chance) {
    /* Negative means "each material's own figure", which is what the app
     * wants; anything else overrides every material alike, which is what a
     * test wants. */
    if (chance < 0) {
        s->scatter = SAND_SCATTER_PER_MATERIAL;
    } else {
        s->scatter = chance > 255 ? 255 : chance;
    }
}

void
sand_set_soak(sand_t* s, int chance) {
    if (chance < 0) {
        s->soak = SAND_SOAK_PER_MATERIAL;
    } else {
        s->soak = chance > 255 ? 255 : chance;
    }
}

void
sand_set_soak_convert(sand_t* s, int period) {
    s->soak_convert = period > 0 ? period : SAND_SOAK_CONVERT_PER_MATERIAL;
}

void
sand_set_decay(sand_t* s, int chance) {
    if (chance < 0) {
        s->decay = SAND_DECAY_PER_MATERIAL;
    } else {
        s->decay = chance > 255 ? 255 : chance;
    }
}

void
sand_set_evaporates(sand_t* s, int chance) {
    if (chance < 0) {
        s->evaporates = SAND_EVAPORATES_PER_MATERIAL;
    } else {
        s->evaporates = chance > 255 ? 255 : chance;
    }
}

void
sand_set_mobility(sand_t* s, int chance) {
    if (chance < 0) {
        s->mobility = SAND_MOBILITY_PER_MATERIAL;
    } else {
        s->mobility = chance > 255 ? 255 : chance;
    }
}

void
sand_set_gas_walk(sand_t* s, bool on) {
    s->gas_walk = on;
}

void
sand_set_flammability(sand_t* s, int chance) {
    if (chance < 0) {
        s->flammability = SAND_FLAMMABILITY_PER_MATERIAL;
    } else {
        s->flammability = chance > 255 ? 255 : chance;
    }
}

void
sand_set_conduction(sand_t* s, int chance) {
    if (chance < 0) {
        s->conduction = SAND_CONDUCTION_PER_MATERIAL;
    } else {
        s->conduction = chance > 255 ? 255 : chance;
    }
}

void
sand_set_boils(sand_t* s, int chance) {
    if (chance < 0) {
        s->boils = SAND_BOILS_PER_MATERIAL;
    } else {
        s->boils = chance > 255 ? 255 : chance;
    }
}

void
sand_set_condenses(sand_t* s, int chance) {
    if (chance < 0) {
        s->condenses = SAND_CONDENSES_PER_MATERIAL;
    } else {
        s->condenses = chance > 255 ? 255 : chance;
    }
}

void
sand_set_lava_cooloff(sand_t* s, int chance) {
    if (chance < 0) {
        s->lava_cooloff = SAND_LAVA_COOLOFF_DEFAULT;
    } else {
        s->lava_cooloff = chance > 255 ? 255 : chance;
    }
}

void
sand_set_fuse_cooldown(sand_t* s, int steps) {
    s->fuse_cooldown = (steps < 0) ? -1 : (steps > 255 ? 255 : steps);
}

void
sand_set_crust(sand_t* s, int chance) {
    s->crust = (chance < 0) ? -1 : (chance > 65535 ? 65535 : chance);
}

void
sand_set_lava_burst(sand_t* s, int chance) {
    if (chance < 0) {
        s->lava_burst = SAND_LAVA_BURST_DEFAULT;
    } else {
        s->lava_burst = chance > 255 ? 255 : chance;
    }
}

void
sand_set_acid_rain(sand_t* s, int chance) {
    if (chance < 0) {
        s->acid_rain = SAND_ACID_RAIN_DEFAULT;
    } else {
        s->acid_rain = chance > 255 ? 255 : chance;
    }
}

void
sand_set_acid_dilute_mass_bias(sand_t* s, int bias) {
    s->acid_dilute_mass_bias = (bias < 0) ? SAND_ACID_DILUTE_MASS_BIAS_DEFAULT : bias;
}

/* Each slide's tilt for hot table rows depends on direction and angle of
 * repose, computed once per step for all 32 rows (MATERIAL_ROWS) using cell
 * >> 3. Reads directly from `materials[]` instead of material_by_id() to
 * include gunpowder's row. TWIN_ROW writes identical repose for ORDINARY
 * materials. */
static void
compute_driven(bool driven[MATERIAL_ROWS][2], const int* slide_a, const int* slide_b, int gx, int gy) {
    for (int m = 0; m < MATERIAL_ROWS; m++) {
        const int repose = materials[m].repose;
        driven[m][0] = driven_by_gravity(slide_a[0], slide_a[1], gx, gy, repose);
        driven[m][1] = driven_by_gravity(slide_b[0], slide_b[1], gx, gy, repose);
    }
}

/* Sweep column order against travel direction for sand_step(). Alternates
 * when gravity is vertical. Outputs step direction, not range;
 * span_x_order() turns it into one. */
static int
sweep_x_order(sand_t* s, int dx) {
    int x_step;
    if (dx > 0) {
        x_step = -1;
    } else if (dx < 0) {
        x_step = 1;
    } else if (s->sweep_flip) {
        x_step = -1;
    } else {
        x_step = 1;
    }
    s->pour_phase++;
    s->sweep_flip = !s->sweep_flip;
    return x_step;
}

/* Sweep against travel so a grain's destination is already swept and it
 * cannot move twice. With dy == 0 no row order gives that on its own, so a
 * liquid-free grid alternates the row order per step and keeps only the
 * diagonal pointing into swept rows. A grid holding liquid keeps the plain
 * ascending order: restricting its diagonals stopped poured water reaching
 * the floor, so there a grain can still slide twice in a step. */
static void
choose_sweep_order(const sand_t* s, int dy, const int** slide_a, const int** slide_b, int* y_from, int* y_to,
                   int* y_step) {
    const bool landscape_safe_sweep = dy == 0 && !s->may_have_liquid;

    *y_step = (dy != 0) ? -dy : (landscape_safe_sweep && (s->step_phase & 1) ? -1 : 1);
    *y_from = (*y_step > 0) ? 0 : s->h - 1;
    *y_to = (*y_step > 0) ? s->h : -1;
    if (landscape_safe_sweep) {
        const int* const landscape_slide = ((*slide_a)[1] == -*y_step) ? *slide_a : *slide_b;
        *slide_a = landscape_slide;
        *slide_b = landscape_slide;
    }
}

/* One bit per materials[] row for the two questions the sweep asks of every
 * cell on the grid, so each reads as a shift out of a word in SRAM instead of
 * dereferencing a struct in flash. The same trade gas_kind_mask makes in
 * sand_gas.c, on a hotter loop: this one runs per cell of every awake block.
 *
 * Eight bytes rather than two 32-entry tables, because MATERIAL_ROWS is 32 and
 * a row index therefore fits a uint32_t exactly. */
static uint32_t sweep_skip_mask;   /* KIND_STATIC and KIND_GAS - not ours */
static uint32_t sweep_liquid_mask; /* KIND_LIQUID - takes the liquid path  */
/* liquid_mask()'s answer, indexed by CELL_MATERIAL rather than by row, kept
 * here so a helper reading whole cells needs no extra argument to carry it. */
static uint16_t sweep_cell_liquid_mask;
static bool sweep_tables_ready;

/* Rebuilt fresh every step by compute_driven(), never carried across one -
 * FILE-STATIC rather than a local of sand_step(), so a chunk-parallel
 * dispatch's core-1 half can read it without a
 * dangling pointer into a caller's stack frame that a timed-out join may
 * have already returned from. */
static bool sweep_driven[MATERIAL_ROWS][2];

static void
build_sweep_tables(void) {
    if (sweep_tables_ready) {
        return;
    }
    for (int r = 0; r < MATERIAL_ROWS; r++) {
        if (materials[r].kind == KIND_STATIC || materials[r].kind == KIND_GAS) {
            sweep_skip_mask |= 1u << r;
        }
        if (materials[r].kind == KIND_LIQUID) {
            sweep_liquid_mask |= 1u << r;
        }
    }
    sweep_cell_liquid_mask = liquid_mask();
    sweep_tables_ready = true;
}

/* One block-row's one-shot verdict, carried by pointer so the question is only
 * ever asked on the liquid branch below - a board with no liquid on it never
 * reaches it. One byte, so a block-row costs one store to arm it. */
typedef enum {
    DEST_UNKNOWN = 0,
    DEST_HAS_ROOM,
    DEST_FULL,
} dest_state_t;

/* Off the grid is no room: give_mass() returns 0 for a NULL destination row
 * exactly as it does for a full cell. */
static inline bool
dest_row_full(const uint8_t* row, int x0, int x1) {
    return row == NULL || span_has_no_liquid_room(row, x0, x1, sweep_cell_liquid_mask);
}

/* Every cell a grain in this block can reach lies in one of three destination
 * rows, within the block's own span plus a cell of margin - the fall and both
 * slides - so one test per distinct row answers for every grain in the block.
 * A sealed basin and the interior of a deep pool answer yes every step. */
_Static_assert((SAND_BLOCK_W & (SAND_BLOCK_W - 1)) == 0, "dest_rows_full() recovers a block's span from x by masking");

static bool
dest_rows_full(const uint8_t* prow, const uint8_t* arow, const uint8_t* brow, int x, int w) {
    const int lo = x & ~(SAND_BLOCK_W - 1);
    const int hi = lo + SAND_BLOCK_W;
    const int x0 = (lo > 0) ? lo - 1 : 0;
    const int x1 = (hi < w) ? hi + 1 : w;

    if (!dest_row_full(prow, x0, x1)) {
        return false;
    }
    if (arow != prow && !dest_row_full(arow, x0, x1)) {
        return false;
    }
    if (brow != prow && brow != arow && !dest_row_full(brow, x0, x1)) {
        return false;
    }
    return true;
}

/* A liquid grain that went nowhere is the cheapest evidence that the rows it
 * reached for are full, and the only thing that buys dest_rows_full() its
 * loads: on a falling column every grain moves and the span is never read.
 * Asked once, since nothing later in the sweep can ADD room. */
static bool
step_one_liquid_grain(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx,
                      int dy, const int* slide_a, const int* slide_b, cell_t grain, dest_state_t* dest) {
    const bool moved = move_liquid_grain(s, row, prow, x, y, dx, dy, slide_a, slide_b, grain, CELL_MATERIAL(grain),
                                         *dest == DEST_FULL);
    if (!moved && *dest == DEST_UNKNOWN) {
        *dest = dest_rows_full(prow, arow, brow, x, w) ? DEST_FULL : DEST_HAS_ROOM;
    }
    return moved;
}

static bool
step_one_grain(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx,
               int dy, const int* slide_a, const int* slide_b, int load_dx, int load_dy, int jostle,
               bool driven[MATERIAL_ROWS][2], dest_state_t* dest) {
    const cell_t grain = row[x];

    /* Two shifts answer what two flash reads used to. Gas is skipped because
     * it moves AGAINST the sweep's direction, into cells not yet visited, and
     * would teleport - sand_step_gas() has its own reversed pass for it.
     *
     * Only a powder reaches materials[] now, and only for its density and
     * scatter, which no mask can carry. */
    const unsigned mrow = (unsigned)grain >> 3;
    if (((sweep_skip_mask >> mrow) & 1u) != 0) {
        return false;
    }
    if (((sweep_liquid_mask >> mrow) & 1u) != 0) {
        return step_one_liquid_grain(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a, slide_b, grain, dest);
    }

    const material_t* mat = material_of(grain);

    const uint8_t density = mat->density;

    if (jostle == 0) {
        const int scatter = (s->scatter >= 0) ? s->scatter : mat->scatter;
        /* _impl, called directly, not the ordinary try_fall_or_scatter()
         * below - this is the hottest call site in the whole simulation,
         * and it needs to stay inlined. See sand_priv.h's own comment
         * above try_fall_or_scatter_impl() for why there are two forms
         * of this function at all. */
        if (try_fall_or_scatter_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a, slide_b, grain, density,
                                     scatter)) {
            return true;
        }
    }

    /* The selected ROW isn't `mat_id`. `try_slide_impl()` uses `driven_row`
     * to index `driven[]` in `pick_slide_order()`, not as `mat_id`. This
     * change affects only MAT_EXTENDED, distinguishing it from gunpowder, due
     * to TWIN_ROW for ORDINARY materials. */
    const uint8_t driven_row = (uint8_t)(grain >> 3);
    return try_slide_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a, slide_b, load_dx, load_dy, jostle, grain,
                          driven_row, density, mat, driven);
}

/* The non-inline forms exist alongside the inline versions (see sand_priv.h
 * comment). sand_gas.c calls these, not the _impl versions, to avoid a second
 * inlined copy. */
bool
try_fall_or_scatter(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx,
                    int dy, const int* slide_a, const int* slide_b, cell_t grain, uint8_t density, int scatter) {
    return try_fall_or_scatter_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a, slide_b, grain, density,
                                    scatter);
}

bool
try_slide(sand_t* s, uint8_t* row, uint8_t* prow, uint8_t* arow, uint8_t* brow, int x, int y, int w, int dx, int dy,
          const int* slide_a, const int* slide_b, int load_dx, int load_dy, int jostle, cell_t grain,
          uint8_t driven_row, uint8_t density, const material_t* mat, bool driven[][2]) {
    return try_slide_impl(s, row, prow, arow, brow, x, y, w, dx, dy, slide_a, slide_b, load_dx, load_dy, jostle, grain,
                          driven_row, density, mat, driven);
}

/* Sleeping off when block_state missing. Wakes blocks if grid shaken or
 * settle direction changes. Returns dithered direction. Compares NEAREST
 * direction for sleeping. Clears BLOCK_ACTIVE each step for finalisation. */
static uint8_t
compute_settled_bit(sand_t* s, int jostle, int dx, int dy, int load_dx, int load_dy) {
    if (s->block_state == NULL) {
        return 0;
    }

    const int n = s->block_cols * s->block_rows;
    const uint8_t bit = (dx == load_dx && dy == load_dy) ? BLOCK_SETTLED_NEAREST : BLOCK_SETTLED_OTHER;
    if (jostle > 0 || load_dx != s->last_load_dx || load_dy != s->last_load_dy) {
        /* A mass wake leaves nothing settled, so the sweep will walk every
         * block and re-establish BLOCK_HAS_LIQUID for all of them - clearing
         * it here along with everything else is exactly right. */
        memset(s->block_state, 0, (size_t)n);
    } else {
        for (int i = 0; i < n; i++) {
            uint8_t v = (uint8_t)(s->block_state[i] & ~BLOCK_ACTIVE);
            /* BLOCK_HAS_LIQUID is the sweep's own observation, so it is
             * cleared for exactly the blocks the sweep is about to make it
             * afresh. A block it will SKIP keeps last time's answer, which is
             * still true: nothing in a settled block moved. See the invariant
             * above BLOCK_HAS_LIQUID in sand_priv.h. */
            if ((v & bit) == 0) {
                v &= (uint8_t)~BLOCK_HAS_LIQUID;
            }
            s->block_state[i] = v;
        }
    }
    return bit;
}

/* A half-open [lo, hi) walked in sweep_x_order()'s direction. Block columns
 * and the cells within one share it, so the two orders cannot drift apart. */
static void
span_x_order(int lo, int hi, int x_step, int* from, int* to, int* step) {
    if (x_step > 0) {
        *from = lo;
        *to = hi;
        *step = 1;
    } else {
        *from = hi - 1;
        *to = lo - 1;
        *step = -1;
    }
}

/* step_one_block() parameters bundled into a struct for efficiency. Reduced
 * call arguments to two (this and bx) to avoid register overflow. Flat list
 * caused performance regression on RISC-V hardware with 8 registers. */
typedef struct {
    sand_t* s;
    uint8_t *row, *prow, *arow, *brow;
    int y, w, dx, dy, x_step;
    const int *slide_a, *slide_b;
    int load_dx, load_dy, jostle;
    int by;
    /* The caller's own columns. A block at either end of them is swept over
     * its overlap with the range, not over the whole block. */
    int x0, x1;
    /* Materials are liquid as a bitmask over the nibble, similar to
     * sand_liquid.c's liquid_mask(): the sweep checks if a cell is liquid to
     * maintain BLOCK_HAS_LIQUID, using a shift-and-mask on a register for
     * efficiency. */
    uint16_t is_liquid;
    bool (*driven)[2];
} sweep_ctx_t;

/* Marks BLOCK_ACTIVE if anything moves in a block's x-span within a row, for
 * compute_settled_bit()'s later finalisation pass; does nothing if
 * block_state is disabled. */
static void
step_one_block(const sweep_ctx_t* ctx, int bx) {
    const int lo = im_max(bx * SAND_BLOCK_W, ctx->x0);
    const int hi = im_min(bx * SAND_BLOCK_W + SAND_BLOCK_W, ctx->x1);

    int cx_from, cx_to, cx_step;
    span_x_order(lo, hi, ctx->x_step, &cx_from, &cx_to, &cx_step);

    dest_state_t dest = DEST_UNKNOWN;

    bool moved_here = false;
    unsigned saw_liquid = 0;
    for (int x = cx_from; x != cx_to; x += cx_step) {
        const cell_t c = ctx->row[x];
        if (CELL_IS_EMPTY(c)) {
            continue;
        }
        /* Accumulated in a register and stored once per block, same shape as
         * moved_here. BLOCK_HAS_LIQUID keeps it true at O(blocks) per step
         * instead of O(moves) - a skip structure earns its cost only when it
         * is questioned before it is built. Counted BEFORE the stamped skip:
         * a cell that arrived here this pass is still liquid sitting in this
         * block, and cross-flow looks nowhere the sweep did not mark. */
        saw_liquid |= (unsigned)(ctx->is_liquid >> CELL_MATERIAL(c)) & 1u;
        if (sand_cell_stamped(ctx->s, x, ctx->y)) {
            continue;
        }
        if (step_one_grain(ctx->s, ctx->row, ctx->prow, ctx->arow, ctx->brow, x, ctx->y, ctx->w, ctx->dx, ctx->dy,
                           ctx->slide_a, ctx->slide_b, ctx->load_dx, ctx->load_dy, ctx->jostle, ctx->driven, &dest)) {
            moved_here = true;
        }
    }

    if ((moved_here || saw_liquid) && ctx->s->block_state != NULL) {
        ctx->s->block_state[ctx->by * ctx->s->block_cols + bx] |=
            (uint8_t)((moved_here ? BLOCK_ACTIVE : 0) | (saw_liquid ? BLOCK_HAS_LIQUID : 0));
    }
}

/* Gravity sweep row, block-column: skip settled blocks. Skipped if
 * settled_bit set, no work needed. When sleeping disabled (block_state NULL),
 * settled_bit 0, no skips, same as cell-by-cell walk. */
static void
step_one_row(sand_t* s, int y, int x0, int x1, int w, int dx, int dy, const int* slide_a, const int* slide_b,
             int x_step, int load_dx, int load_dy, int jostle, uint8_t settled_bit, uint16_t is_liquid,
             bool driven[MATERIAL_ROWS][2]) {
    sweep_ctx_t ctx = {
        .s = s,
        .row = s->cells + (size_t)y * (size_t)w,
        .prow = dest_row(s, y + dy),
        .arow = dest_row(s, y + slide_a[1]),
        .brow = dest_row(s, y + slide_b[1]),
        .y = y,
        .w = w,
        .dx = dx,
        .dy = dy,
        .x_step = x_step,
        .slide_a = slide_a,
        .slide_b = slide_b,
        .load_dx = load_dx,
        .load_dy = load_dy,
        .jostle = jostle,
        .by = y / SAND_BLOCK_H,
        .x0 = x0,
        .x1 = x1,
        .is_liquid = is_liquid,
        .driven = driven,
    };

    int bx_from, bx_to, bx_step;
    span_x_order(x0 / SAND_BLOCK_W, (x1 + SAND_BLOCK_W - 1) / SAND_BLOCK_W, x_step, &bx_from, &bx_to, &bx_step);

    for (int bx = bx_from; bx != bx_to; bx += bx_step) {
        if (settled_bit != 0 && (s->block_state[ctx.by * s->block_cols + bx] & settled_bit)) {
            continue;
        }
        step_one_block(&ctx, bx);
    }
}

/* Finalise a step's settling: a block earns the settled bit if no
 * BLOCK_ACTIVE marks exist in the step or its neighbours. Deferred per
 * block, not row, since a block spans SAND_BLOCK_H rows.
 *
 * ORDER-INDEPENDENT over [by_from, by_to): every iteration only reads
 * BLOCK_ACTIVE, which nothing here writes, and only writes its own block's
 * settled bits - never a neighbour's - so a range may run before, after or
 * genuinely alongside any other, with no guard. */
static void
finalize_settling_range(sand_t* s, uint8_t settled_bit, int by_from, int by_to) {
    for (int by = by_from; by < by_to; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            const int i = by * s->block_cols + bx;
            if (s->block_state[i] & BLOCK_ACTIVE) {
                continue;
            }
            if (any_neighbor_active(s, bx, by)) {
                s->block_state[i] &= (uint8_t)~(BLOCK_SETTLED_NEAREST | BLOCK_SETTLED_OTHER);
            } else {
                s->block_state[i] |= settled_bit;
            }
        }
    }
}

typedef struct {
    sand_t* s;
    uint8_t settled_bit;
    int by_from, by_to;
} finalize_settling_half_t;

_Static_assert(sizeof(finalize_settling_half_t) <= JOB_CTX_MAX, "finalize_settling_half_t must fit JOB_CTX_MAX");

static void
finalize_settling_worker(void* ctx) {
    const finalize_settling_half_t* half = ctx;
    finalize_settling_range(half->s, half->settled_bit, half->by_from, half->by_to);
}

/* Below this many block rows, a single core walks the whole board faster
 * than a hop to core 1 and back costs. */
#define FINALIZE_SETTLING_SPLIT_MIN_BLOCK_ROWS 4

static void
finalize_settling(sand_t* s, uint8_t settled_bit) {
    if (s->block_state == NULL) {
        return;
    }

    if (sand_two_core_step_enabled() && s->block_rows >= FINALIZE_SETTLING_SPLIT_MIN_BLOCK_ROWS) {
        const int mid = s->block_rows / 2;
        finalize_settling_half_t half = {s, settled_bit, mid, s->block_rows};
        (void)job_run_core1(finalize_settling_worker, &half, sizeof half);
        finalize_settling_range(s, settled_bit, 0, mid);
        (void)job_wait(100);
        return;
    }

    finalize_settling_range(s, settled_bit, 0, s->block_rows);
}

/* The two rays a liquid levels along, and what one step of each costs in
 * gravitational potential. See xflow_t. */
static void
build_xflow(xflow_t* f, int gx, int gy) {
    const int ax = im_abs(gx), ay = im_abs(gy);
    const int sx = im_sign(gx), sy = im_sign(gy);

    /* Whichever of gx/gy has the larger magnitude picks the major ray:
     * `ax` runs perpendicular to that axis, `dg` built from the same two
     * signs (not looked up), so it is always the diagonal beside the
     * lean, never the far one. `q_q8` is the raw ratio of the smaller
     * gravity component to the larger (0-256) - the TANGENT of the tilt,
     * deliberately not diagonal_weight()'s angle correction (that answers
     * where the true angle sits between octants for dithering gravity,
     * not the slope wanted here). */
    if (ay >= ax) {
        /* Gravity is mostly vertical: the level surface runs mostly across. */
        f->ax[0] = (sx >= 0) ? 1 : -1;
        f->ax[1] = 0;
        f->dg[0] = f->ax[0];
        f->dg[1] = (sy >= 0) ? -1 : 1;
        f->q_q8 = (ay != 0) ? (ax * 256) / ay : 0;
    } else {
        /* Gravity is mostly sideways: the level surface runs mostly up. */
        f->ax[0] = 0;
        f->ax[1] = (sy >= 0) ? -1 : 1;
        f->dg[0] = (sx >= 0) ? 1 : -1;
        f->dg[1] = f->ax[1];
        f->q_q8 = (ay * 256) / ax;
    }

    /* im_len()'s ~4% approximation (intmath.h) is fine: nothing reads a
     * bias to that precision, only sign and rough size. A bias of exactly
     * zero when a ray is exactly level still holds - it comes from the
     * dot product cancelling exactly, independent of length. */
    const int len = im_len(gx, gy);
    if (len == 0) {
        f->bias_ax_q8 = 0;
        f->bias_dg_q8 = 0;
        return;
    }
    /* Both biases share one pair of per-axis constants, `bx`/`by`, rather
     * than each worked out separately - that is what keeps the level
     * field consistent: find_shallowest() compares cells reachable by
     * either ray, and a shared (bx, by) guarantees those comparisons
     * agree regardless of which ray got each cell there. */
    const int bx = (MASS_MAX * 256 * gx) / len;
    const int by = (MASS_MAX * 256 * gy) / len;
    f->bias_ax_q8 = bx * f->ax[0] + by * f->ax[1];
    f->bias_dg_q8 = bx * f->dg[0] + by * f->dg[1];
}

/* PINNED at 16, not left to the compiler: an unpinned attribute can bind to
 * whatever definition follows it rather than to this function, letting an
 * unrelated change silently shift sand_step()'s alignment and regress
 * performance. 32 costs about 4.5% on liquid-free controls for nothing; 16 is
 * near-free and still starts within a 32-byte cache block. */

/* CHECK WITH objdump, NOT the diff: .text.sand_step should read 2**4. Binding
 * to the wrong symbol still compiles and passes everything. */
/* A mobility of 0 or 255 always admits the move; only a value between them
 * draws a roll. Asked of the global override when it is set, and otherwise of
 * every liquid actually present. */
static bool
viscous_liquid_possible(const sand_t* s) {
    if (s->mobility >= 0) {
        return s->mobility != 0 && s->mobility < 255;
    }
    for (int m = 0; m < MATERIAL_MAX; m++) {
        if ((s->may_have_materials & (1u << m)) == 0) {
            continue;
        }
        const material_t* mat = material_by_id((material_id_t)m);
        if (mat->kind == KIND_LIQUID && mat->mobility != 0 && mat->mobility < 255) {
            return true;
        }
    }
    return false;
}

/* Whether every block overlapping [x0, x1) x [y0, y1) already carries
 * `settled_bit`. Blocks reaching past that rectangle count too: a settled
 * answer then holds for strictly more than the caller asked about, which is
 * the safe direction for a skip. */
static bool
blocks_settled_over(const sand_t* s, int x0, int x1, int y0, int y1, uint8_t settled_bit) {
    if (settled_bit == 0) {
        return false;
    }
    const int bx1 = (x1 - 1) / SAND_BLOCK_W;
    const int by1 = (y1 - 1) / SAND_BLOCK_H;

    for (int by = y0 / SAND_BLOCK_H; by <= by1; by++) {
        const uint8_t* const brow = &s->block_state[(size_t)by * (size_t)s->block_cols];
        for (int bx = x0 / SAND_BLOCK_W; bx <= bx1; bx++) {
            if ((brow[bx] & settled_bit) == 0) {
                return false;
            }
        }
    }
    return true;
}

/* The gravity sweep's own inner loop, over the columns [x0, x1) of the rows
 * [y0, y1) in y_step order - shared by the plain serial call below and every
 * chunk a lane takes off the schedule. */
static void
sweep_range(sand_t* s, int y0, int y1, int y_step, int x0, int x1, int w, int dx, int dy, const int* slide_a,
            const int* slide_b, int x_step, int load_dx, int load_dy, int jostle, uint8_t settled_bit,
            uint16_t is_liquid) {
    int scanned_by = -1;
    bool block_row_settled = false;

    for (int y = y0; y != y1; y += y_step) {
        if (settled_bit != 0) {
            const int by = y / SAND_BLOCK_H;
            if (by != scanned_by) {
                scanned_by = by;
                block_row_settled = blocks_settled_over(s, x0, x1, y, y + 1, settled_bit);
            }
            if (block_row_settled) {
                continue;
            }
        }
        step_one_row(s, y, x0, x1, w, dx, dy, slide_a, slide_b, x_step, load_dx, load_dy, jostle, settled_bit,
                     is_liquid, sweep_driven);
    }
}

/* Hashed draws are armed by a split pass, so a serial step and a split step
 * of the same scene draw different numbers and their boards diverge on the
 * RNG rather than on ordering. A comparison of the two arms this first. */
static bool sand_force_hashed_rng_on;

void
sand_force_hashed_rng(bool on) {
    sand_force_hashed_rng_on = on;
}

/* The cut, the order and the callback one split pass runs on, file-static
 * rather than a frame of its caller: a lane that misses its join keeps
 * reading here, and a frame the caller has already returned from would be
 * gone. Passes never overlap, so one set serves them all. */
static sand_chunk_plan_t chunk_plan;
static sand_chunk_sched_t chunk_sched;
static sand_chunk_fn_t chunk_pass_fn;
static void* chunk_pass_arg;

/* A spin is one poll of up to eight neighbour bytes, so this outlasts by a
 * wide margin the chunk a lane waits on, and still bounds the wait: past it
 * the caller finishes the board alone. */
#define CHUNK_PASS_SPIN_LIMIT 20000u
#define CHUNK_PASS_JOIN_MS    100u

void
sand_chunk_pass_cells(int cx, int cy, int* x0, int* x1, int* y0, int* y1) {
    sand_chunk_cells(&chunk_plan, cx, cy, x0, x1, y0, y1);
}

static void
chunk_pass_lane1_worker(void* ctx) {
    (void)ctx;
    sand_chunk_run_lane(&chunk_sched, 1, CHUNK_PASS_SPIN_LIMIT, chunk_pass_fn, chunk_pass_arg);
}

/* A failed wait means core 1 is still inside a chunk: a few chunks left
 * unstepped for one step is recoverable, two threads in neighbouring chunks
 * is not, so the rest of the board waits for the next step instead. */
static void
join_chunk_pass_lane1(void) {
    if (job_wait(CHUNK_PASS_JOIN_MS)) {
        sand_chunk_run_rest(&chunk_sched, chunk_pass_fn, chunk_pass_arg);
        return;
    }
    chunk_sched.abort = 1;
    if (job_wait(CHUNK_PASS_JOIN_MS)) {
        sand_chunk_run_rest(&chunk_sched, chunk_pass_fn, chunk_pass_arg);
    }
}

static sand_chunk_pass_driver_t chunk_pass_driver;

void
sand_chunk_pass_set_driver_for_test(sand_chunk_pass_driver_t driver) {
    chunk_pass_driver = driver;
}

static bool
step_one_chunk_pass_round(int round) {
    if (chunk_pass_driver == SAND_CHUNK_PASS_ALTERNATE) {
        const bool first = sand_chunk_step_lane(&chunk_sched, round & 1, chunk_pass_fn, chunk_pass_arg);
        return sand_chunk_step_lane(&chunk_sched, 1 - (round & 1), chunk_pass_fn, chunk_pass_arg) || first;
    }

    const int eager = (chunk_pass_driver == SAND_CHUNK_PASS_LANE1_EAGER) ? 1 : 0;
    bool moved = false;
    while (sand_chunk_step_lane(&chunk_sched, eager, chunk_pass_fn, chunk_pass_arg)) {
        moved = true;
    }
    return sand_chunk_step_lane(&chunk_sched, 1 - eager, chunk_pass_fn, chunk_pass_arg) || moved;
}

/* One thread standing in for two, so a test can ask whether the board the
 * schedule produces depends on how the lanes interleave. */
static void
drive_chunk_pass_lanes_by_hand(void) {
    for (int round = 0; round < 2 * SAND_CHUNKS_MAX && step_one_chunk_pass_round(round); round++) {}
    sand_chunk_run_rest(&chunk_sched, chunk_pass_fn, chunk_pass_arg);
}

static void
drive_chunk_pass_lanes(void) {
    const bool by_hand = chunk_pass_driver != SAND_CHUNK_PASS_CORE1 && chunk_pass_driver != SAND_CHUNK_PASS_SOLO;

    if (chunk_pass_driver == SAND_CHUNK_PASS_CORE1 && job_try_core1(chunk_pass_lane1_worker, NULL, 0)) {
        sand_chunk_run_lane(&chunk_sched, 0, CHUNK_PASS_SPIN_LIMIT, chunk_pass_fn, chunk_pass_arg);
        join_chunk_pass_lane1();
    } else if (by_hand) {
        drive_chunk_pass_lanes_by_hand();
    } else {
        sand_chunk_run_rest(&chunk_sched, chunk_pass_fn, chunk_pass_arg);
    }
}

bool
sand_chunk_pass_ready(const sand_t* s) {
    sand_chunk_plan_t fits;

    return sand_two_core_step_enabled() && s->lane_scratch != NULL
           && sand_chunk_plan(&fits, s->w, s->h, sand_chunk_side(s), 0, 0);
}

unsigned sand_split_sequential_draws;

/* xorshift32 is a bijection, so replaying it from the state a pass started on
 * recovers exactly how many draws were taken from it. Bounded because an
 * unreachable state would otherwise never terminate. */
#define SEQUENTIAL_DRAW_SCAN_MAX 65536u

static void
count_sequential_draws(uint32_t before, uint32_t after) {
    if (after == before) {
        return;
    }
    rng_t replay = {before};
    for (unsigned n = 1; n <= SEQUENTIAL_DRAW_SCAN_MAX; n++) {
        if (rng_next(&replay) == after) {
            sand_split_sequential_draws += n;
            return;
        }
    }
    sand_split_sequential_draws += SEQUENTIAL_DRAW_SCAN_MAX;
}

bool
sand_chunk_pass_ranks_later(int x0, int y0, int x1, int y1) {
    const int side = chunk_plan.side;
    const int from = ((y0 + chunk_plan.off_y) / side) * chunk_plan.cols + (x0 + chunk_plan.off_x) / side;
    const int to = ((y1 + chunk_plan.off_y) / side) * chunk_plan.cols + (x1 + chunk_plan.off_x) / side;

    return chunk_sched.order.rank[to] > chunk_sched.order.rank[from];
}

bool
sand_chunk_pass_run(sand_t* s, int tx, int ty, sand_chunk_pass_stamps_t stamps, sand_chunk_fn_t fn, void* pass) {
    sand_lane_t* const lanes = sand_lanes(s);

    if (!sand_chunk_pass_ready(s) || lanes == NULL) {
        return false;
    }
    (void)sand_chunk_plan(&chunk_plan, s->w, s->h, sand_chunk_side(s), 0, 0);
    sand_chunk_order(&chunk_sched.order, chunk_plan.cols, chunk_plan.rows, tx, ty, s->step_phase);
    chunk_sched.cols = chunk_plan.cols;
    chunk_sched.rows = chunk_plan.rows;
    sand_chunk_sched_reset(&chunk_sched);
    chunk_pass_fn = fn;
    chunk_pass_arg = pass;

    s->rng_hashed = true;
    if (stamps == SAND_CHUNK_PASS_STAMP_CROSSINGS) {
        sand_stamps_arm(s);
    }
    for (int i = 0; i < SAND_LANE_COUNT; i++) {
        sand_lane_prepare(&lanes[i], s);
    }

    const uint32_t rng_at_prepare = s->rng.state;
    drive_chunk_pass_lanes();
    count_sequential_draws(rng_at_prepare, s->rng.state);

    for (int i = 0; i < SAND_LANE_COUNT; i++) {
        count_sequential_draws(rng_at_prepare, lanes[i].local.rng.state);
        sand_lane_merge(s, &lanes[i]);
    }
#ifdef DEVICE_BUILD
    s->split_lane_aborts += (chunk_sched.abort != 0);
#endif
    if (stamps == SAND_CHUNK_PASS_STAMP_CROSSINGS) {
        sand_stamps_disarm(s);
    }
    s->rng_hashed = false;
    return true;
}

/* Chunks this step's sweep did not skip, counted per lane and summed once
 * both have joined. */
unsigned sand_sweep_chunks_swept;

typedef struct {
    sand_lane_t* lanes;
    int w, dx, dy, x_step, load_dx, load_dy, jostle;
    int slide_a[2], slide_b[2];
    uint16_t is_liquid;
    uint8_t settled_bit;
    int y_step;
    unsigned swept[SAND_LANE_COUNT];
} sweep_pass_t;

static sweep_pass_t sweep_pass;

static void
sweep_one_chunk(void* pass, int lane, int cx, int cy) {
    sweep_pass_t* const c = pass;
    sand_t* const view = &c->lanes[lane].local;
    int x0, x1, y0, y1;

    sand_chunk_pass_cells(cx, cy, &x0, &x1, &y0, &y1);
    if (blocks_settled_over(view, x0, x1, y0, y1, c->settled_bit)) {
        return;
    }
    c->swept[lane]++;

    const int from = (c->y_step > 0) ? y0 : y1 - 1;
    const int to = (c->y_step > 0) ? y1 : y0 - 1;
    sweep_range(view, from, to, c->y_step, x0, x1, c->w, c->dx, c->dy, c->slide_a, c->slide_b, c->x_step, c->load_dx,
                c->load_dy, c->jostle, c->settled_bit, c->is_liquid);
}

/* Travel is the dithered direction the sweep itself uses, so the chunk
 * holding a move's destination is always settled first. */
static void
run_sweep_split(sand_t* s, int dx, int dy) {
    if (!sand_chunk_pass_run(s, im_sign(dx), im_sign(dy), SAND_CHUNK_PASS_STAMP_CROSSINGS, sweep_one_chunk,
                             &sweep_pass)) {
        return;
    }
    for (int i = 0; i < SAND_LANE_COUNT; i++) {
        sand_sweep_chunks_swept += sweep_pass.swept[i];
    }
}

__attribute__((aligned(16))) void
sand_step(sand_t* s, int gx, int gy, int jostle) {
    /* Emitters act first per step, before gravity, mimicking
     * sand_spawn_cell() calls. This allows new grains to move immediately.
     * Runs unconditionally, even in free fall, ensuring "once per
     * sand_step()". */
    build_sweep_tables();

#ifdef DEVICE_BUILD
    memset(&s->pass_us, 0, sizeof s->pass_us);
#endif
    s->explosions_this_step = 0;
    s->confined_blasts_this_step = 0;

    s->step_phase++;
    s->may_have_viscous_liquid = viscous_liquid_possible(s);

    emit_from_emitters(s);

    /* Dithered rather than nearest, so a tilt between two of the eight
     * directions flows at its true angle instead of snapping. Costs one random
     * number per STEP - not per grain - so it is free at this scale. */
    int dx, dy;
    sand_gravity_direction_dithered(s, gx, gy, &dx, &dy);

    /* Load measures directionally, ignoring dithering. Grain weight, a pile
     * property, stays constant despite step rounding. Dithering makes
     * diagonal steps appear empty above vertical columns, treating buried
     * grains as free surface grains roughly one in eight, shifting pile bases
     * sideways. */
    int load_dx, load_dy;
    sand_gravity_direction(gx, gy, &load_dx, &load_dy);

    if (dx == 0 && dy == 0) {
        return; /* free fall: no down, so nothing settles */
    }

    const int i = ring_of(dx, dy);
    const int* slide_a = ring_dir(i + 7);
    const int* slide_b = ring_dir(i + 1);

    const uint8_t settled_bit = compute_settled_bit(s, jostle, dx, dy, load_dx, load_dy);

    /* A body held up under one gravity can be loose under the next, and a turn
     * that moves no cell marks no row - so mark_rows() cannot be what re-arms
     * the fall pass here. */
    if (load_dx != s->last_load_dx || load_dy != s->last_load_dy) {
        s->faller_may_move = true;
    }

    /* Written AFTER compute_settled_bit() returns, and OUTSIDE it: this is
     * a fact about the board's settled direction, not about sleeping
     * bookkeeping, so it must not depend on block sleeping being on -
     * anything else asking which way is down (growth, for one) needs it
     * on any grid, with or without block_state. */
    s->last_load_dx = load_dx;
    s->last_load_dy = load_dy;
    s->last_step_dx = dx;
    s->last_step_dy = dy;

    int y_from, y_to, y_step;
    choose_sweep_order(s, dy, &slide_a, &slide_b, &y_from, &y_to, &y_step);

    compute_driven(sweep_driven, slide_a, slide_b, gx, gy);

    const int x_step = sweep_x_order(s, dx);

    /* Liquid spreads PERPENDICULAR TO GRAVITY, not across screen. Tilt
     * affects direction. Use nearest, not dithered, for stability. See
     * equalise_liquids() and test_a_settled_pool_does_not_flicker. */
    const int i_stable = ring_of(load_dx, load_dy);
    const int* const perp_a = ring_dir(i_stable + 2);
    const int* const perp_b = ring_dir(i_stable + 6);

    xflow_t flow;
    build_xflow(&flow, gx, gy);

    const int w = s->w;
    const uint16_t is_liquid = liquid_mask();

    /* Hashed draws (sand_rng_next_at(), sand_priv.h) are armed for exactly
     * this window, never longer - gas and reactions later this step must
     * still draw from the plain sequential stream. */
#ifdef DEVICE_BUILD
    const int64_t sweep_t0 = esp_timer_get_time();
#endif
    if (sand_chunk_pass_ready(s)) {
        sweep_pass = (sweep_pass_t){
            .lanes = sand_lanes(s),
            .w = w,
            .dx = dx,
            .dy = dy,
            .x_step = x_step,
            .load_dx = load_dx,
            .load_dy = load_dy,
            .jostle = jostle,
            .slide_a = {slide_a[0], slide_a[1]},
            .slide_b = {slide_b[0], slide_b[1]},
            .is_liquid = is_liquid,
            .settled_bit = settled_bit,
            .y_step = y_step,
        };
        run_sweep_split(s, dx, dy);
    } else {
        s->rng_hashed = sand_force_hashed_rng_on;
        sweep_range(s, y_from, y_to, y_step, 0, w, w, dx, dy, slide_a, slide_b, x_step, load_dx, load_dy, jostle,
                    settled_bit, is_liquid);
        s->rng_hashed = false;
    }
#ifdef DEVICE_BUILD
    s->pass_us.sweep_us = esp_timer_get_time() - sweep_t0;
#endif

    /* Cross-flow for liquids, excluding gravity. See sand_step_liquids() in
     * sand_liquid.c. Runs before finalising block sleep states to ensure
     * BLOCK_ACTIVE reflects entire step. */
    sand_step_liquids(s, &flow, dx, dy);

    /* Rising gas doesn't join main sweep. Order of sand_step_liquids()
     * doesn't matter; both must finish before finalize_settling(). Checked
     * here, not via sand_step_gas()'s early return. Called every step,
     * skipping avoids marshalling nine arguments if no gas. Flash layout
     * cost. */
    if (s->may_have_gas) {
#ifdef DEVICE_BUILD
        const int64_t gas_t0 = esp_timer_get_time();
#endif
        sand_step_gas(s, gx, gy, dx, dy, slide_a, slide_b, perp_a, perp_b, load_dx, load_dy, x_step, jostle);
#ifdef DEVICE_BUILD
        s->pass_us.gas_us = esp_timer_get_time() - gas_t0;
#endif
    }

    /* Same slot for burning cell reactions; ignition/extinguish/burn-out are
     * not gravity-ward or movement. Must finish before finalize_settling().
     * Takes `s` argument, unlike sand_step_gas(). Boiling now happens at heat
     * source. No cost to dodge by checking may_have_burning, internal check
     * suffices. */
#ifdef DEVICE_BUILD
    const int64_t reactions_t0 = esp_timer_get_time();
#endif
    sand_step_reactions(s);
#ifdef DEVICE_BUILD
    s->pass_us.reactions_us = esp_timer_get_time() - reactions_t0;
#endif

    /* Final step after others to ensure correct position and arc for thrown
     * grains, adding outward half after gravity. */
#ifdef DEVICE_BUILD
    const int64_t impulses_t0 = esp_timer_get_time();
#endif
    step_impulses(s, dx, dy);
#ifdef DEVICE_BUILD
    s->pass_us.impulses_us = esp_timer_get_time() - impulses_t0;
#endif

    finalize_settling(s, settled_bit);
}
