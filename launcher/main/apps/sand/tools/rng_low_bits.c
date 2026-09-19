/*
 * Measures whether masking the LOW bits of a draw gives correlated rolls,
 * and what a sealed gas pocket with one exit actually does.
 *
 * Almost every chance in this app is `draw & 0xFF < chance`, so a rare
 * roll is only as rare as the generator's bottom eight bits make it. A
 * pocket reaching for a 2-in-256 direction kept some seeds inside for ten
 * thousand steps, which is either a property of the generator or a
 * property of the scene, and nothing in the tree could tell the two apart.
 *
 * Four measurements, in order of distance from the real code: the raw
 * stream's low and high byte alone; the joint distribution of one draw's
 * low byte with the next one's, since a roll gated behind another roll is
 * conditioned on its predecessor; the same stream decimated by the number
 * of draws a step consumes; and then the pocket itself, stepped through
 * sand_step() with every draw recovered from the generator state either
 * side of the step.
 *
 * Reconstruction rather than a hook: an instrument compiled into the
 * simulation would be measuring a different build from the one that ships.
 * Replaying xorshift32 forward from where the state was to where it ended
 * recovers each draw in between exactly, and the draws pair up as the gas
 * mover makes them - a mobility roll, then a direction roll if it passed.
 *
 * Deterministic: fixed seeds, fixed step counts, no clock read.
 *
 * Usage:
 *   main/apps/sand/tools/report_rng_low_bits.sh
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "material.h"
#include "sand.h"
#include "util/rng.h"

/* The suite's own sealed-pocket geometry on the suite's own 8x8 grid: a
 * 3x3 stone ring with gas in the middle and one corner opened, so the
 * scene stepped here is the scene that raised the question. */
enum { POCKET_W = 8, POCKET_H = 8, POCKET_GAS_X = 3, POCKET_GAS_Y = 3, POCKET_EXIT_X = 2, POCKET_EXIT_Y = 4 };

enum { POCKET_SEEDS = 16, POCKET_STEPS = 10000, POCKET_MOBILITY = 255 };

/* Long enough that a 1-in-256 bucket still holds ~16k samples, short
 * enough that the whole report runs in seconds. */
enum { STREAM_DRAWS = 4000000 };

/* No period below this survives comparison over a window far longer than
 * the pocket run. */
enum { PERIOD_LIMIT = 65536, PERIOD_WINDOW = 262144 };

/* A step drawing more than this has gone somewhere the reconstruction
 * does not understand, and the run says so rather than guessing. */
enum { DRAWS_PER_STEP_MAX = 64 };

/* gas_walk_offset[] in sand_gas.c gives a lower diagonal for a roll under
 * four, and only one of the two lower diagonals is the open corner. */
enum { WALK_LOWER_DIAGONALS = 4, WALK_EXIT_ROLLS = 2 };

#define POCKET_STONE CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT)
#define POCKET_GAS   CELL_MAKE(MAT_GAS, 8)

/* Chi-square against a flat expectation. 255 degrees of freedom for a
 * byte, 255 again for a 16x16 joint table: p=0.001 sits at 330.5. */
static double
chi_square_flat(const uint32_t* counts, int bins, double total) {
    const double expected = total / (double)bins;
    double chi = 0.0;
    for (int i = 0; i < bins; i++) {
        const double d = (double)counts[i] - expected;
        chi += d * d / expected;
    }
    return chi;
}

static void
byte_histograms(uint32_t seed, uint32_t* low, uint32_t* high) {
    rng_t r;
    rng_seed(&r, seed);
    memset(low, 0, sizeof(uint32_t) * 256);
    memset(high, 0, sizeof(uint32_t) * 256);
    for (int i = 0; i < STREAM_DRAWS; i++) {
        const uint32_t x = rng_next(&r);
        low[x & 0xFFu]++;
        high[(x >> 24) & 0xFFu]++;
    }
}

/* The shortest p for which the low-byte sequence repeats over the whole
 * window, or 0 if none does. A short period here would mean the eight bits
 * every chance in this app reads cycle long before the state does. */
static int
low_byte_period(uint32_t seed) {
    uint8_t* bytes = malloc(PERIOD_WINDOW + PERIOD_LIMIT);
    if (bytes == NULL) {
        return -1;
    }
    rng_t r;
    rng_seed(&r, seed);
    for (int i = 0; i < PERIOD_WINDOW + PERIOD_LIMIT; i++) {
        bytes[i] = (uint8_t)(rng_next(&r) & 0xFFu);
    }
    int found = 0;
    for (int p = 1; p <= PERIOD_LIMIT && found == 0; p++) {
        int ok = 1;
        for (int i = 0; i < PERIOD_WINDOW && ok != 0; i++) {
            if (bytes[i] != bytes[i + p]) {
                ok = 0;
            }
        }
        if (ok != 0) {
            found = p;
        }
    }
    free(bytes);
    return found;
}

/* 16x16 over the two bytes' high nibbles. Binning rather than a full
 * 256x256 table keeps the expected count per cell high enough at this
 * sample size for the statistic to mean anything. */
static double
joint_chi_square(uint32_t seed, int decimate, int shift_a, int shift_b) {
    uint32_t joint[256];
    memset(joint, 0, sizeof joint);
    rng_t r;
    rng_seed(&r, seed);
    uint32_t prev = rng_next(&r);
    uint32_t pairs = 0;
    for (int i = 0; i < STREAM_DRAWS; i++) {
        uint32_t cur = 0;
        for (int d = 0; d < decimate; d++) {
            cur = rng_next(&r);
        }
        const uint32_t a = ((prev >> shift_a) & 0xFFu) >> 4;
        const uint32_t b = ((cur >> shift_b) & 0xFFu) >> 4;
        joint[a * 16u + b]++;
        pairs++;
        prev = cur;
    }
    return chi_square_flat(joint, 256, (double)pairs);
}

/* The pocket's own question, asked of the raw stream: how often the draw
 * after a passing mobility roll lands in a lower diagonal, and whether
 * that rate moves once the previous draw is conditioned on. */
static void
conditional_rates(uint32_t seed, int decimate, double* unconditional, double* given_mobility_passes) {
    rng_t r;
    rng_seed(&r, seed);
    uint32_t prev = rng_next(&r);
    uint32_t hits = 0;
    uint32_t total = 0;
    uint32_t cond_hits = 0;
    uint32_t cond_total = 0;
    for (int i = 0; i < STREAM_DRAWS; i++) {
        uint32_t cur = 0;
        for (int d = 0; d < decimate; d++) {
            cur = rng_next(&r);
        }
        const uint32_t hit = ((cur & 0xFFu) < (uint32_t)WALK_LOWER_DIAGONALS) ? 1u : 0u;
        hits += hit;
        total++;
        if ((prev & 0xFFu) < (uint32_t)POCKET_MOBILITY) {
            cond_hits += hit;
            cond_total++;
        }
        prev = cur;
    }
    *unconditional = (double)hits / (double)total;
    *given_mobility_passes = (cond_total != 0) ? (double)cond_hits / (double)cond_total : 0.0;
}

static void
build_sealed_pocket(sand_t* s) {
    sand_clear(s);
    sand_set_mobility(s, POCKET_MOBILITY);
    sand_set_scatter(s, 0);
    sand_set(s, POCKET_GAS_X, POCKET_GAS_Y, POCKET_GAS);
    for (int y = 2; y <= 4; y++) {
        for (int x = 2; x <= 4; x++) {
            if (x != POCKET_GAS_X || y != POCKET_GAS_Y) {
                sand_set(s, x, y, POCKET_STONE);
            }
        }
    }
    sand_set(s, POCKET_EXIT_X, POCKET_EXIT_Y, SAND_EMPTY);
}

/* Replays xorshift32 forward from `before` until it reaches `after`,
 * recording what it passed through: the draws that step made, in order.
 * Negative if the state did not come back within the bound. */
static int
replay_draws(uint32_t before, uint32_t after, uint32_t* out, int max) {
    if (before == after) {
        return 0;
    }
    uint32_t x = before;
    for (int i = 0; i < max; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = x;
        if (x == after) {
            return i + 1;
        }
    }
    return -1;
}

/* A step's draws are one pair per gas cell the rise sweep visits - a
 * mobility roll, then a direction roll only if it passed. A cell moving
 * into a row the sweep has not reached yet is visited again the same
 * step, so a step can hold several pairs. */
static int
first_walk_roll(const uint32_t* draws, int n) {
    if (n < 1) {
        return -1;
    }
    if ((int)(draws[0] & 0xFFu) >= POCKET_MOBILITY) {
        return -1;
    }
    return (n >= 2) ? (int)(draws[1] & 0xFFu) : -1;
}

typedef struct {
    int detect_step;   /* the suite's own criterion: gas anywhere at y >= exit */
    int breakout_step; /* first step that left the gas outside the pocket */
    int exit_hits;     /* direction rolls that picked the open corner */
    int steps_run;
    int steps_at_origin;
    int bad_steps;
    int min_y_after_breakout;
    int max_y_after_breakout;
    uint32_t origin_walk_hist[256];
    uint32_t origin_walks;
} pocket_result_t;

static int
gas_at_or_below_exit(const sand_t* s) {
    for (int y = POCKET_EXIT_Y; y < POCKET_H; y++) {
        for (int x = 0; x < POCKET_W; x++) {
            if (CELL_MATERIAL(sand_at(s, x, y)) == MAT_GAS) {
                return 1;
            }
        }
    }
    return 0;
}

static int
gas_row(const sand_t* s) {
    for (int y = 0; y < POCKET_H; y++) {
        for (int x = 0; x < POCKET_W; x++) {
            if (CELL_MATERIAL(sand_at(s, x, y)) == MAT_GAS) {
                return y;
            }
        }
    }
    return -1;
}

static int
gas_is_in_pocket(const sand_t* s) {
    return CELL_MATERIAL(sand_at(s, POCKET_GAS_X, POCKET_GAS_Y)) == MAT_GAS;
}

/* Runs the pocket for the full step budget whatever the suite's criterion
 * says, so a gas cell that left the pocket without ever being seen at
 * y >= exit is still counted as having left. */
static void
run_pocket(uint32_t seed, pocket_result_t* out) {
    static uint8_t cells[POCKET_W * POCKET_H];
    sand_t s;
    uint32_t draws[DRAWS_PER_STEP_MAX];

    memset(out, 0, sizeof *out);
    out->min_y_after_breakout = POCKET_H;
    out->max_y_after_breakout = -1;

    sand_init(&s, cells, POCKET_W, POCKET_H, seed);
    build_sealed_pocket(&s);

    for (int step = 1; step <= POCKET_STEPS; step++) {
        const int started_in_pocket = gas_is_in_pocket(&s);
        const uint32_t before = s.rng.state;
        sand_step(&s, 0, 1000, 0);
        const int n = replay_draws(before, s.rng.state, draws, DRAWS_PER_STEP_MAX);

        out->steps_run = step;
        if (n < 0) {
            out->bad_steps++;
        } else if (started_in_pocket != 0) {
            const int roll = first_walk_roll(draws, n);
            out->steps_at_origin++;
            if (roll >= 0) {
                out->origin_walk_hist[roll]++;
                out->origin_walks++;
                if (roll < WALK_EXIT_ROLLS) {
                    out->exit_hits++;
                }
            }
        }

        if (gas_is_in_pocket(&s) == 0 && out->breakout_step == 0) {
            out->breakout_step = step;
        }
        if (out->breakout_step != 0) {
            const int y = gas_row(&s);
            if (y >= 0) {
                out->min_y_after_breakout = (y < out->min_y_after_breakout) ? y : out->min_y_after_breakout;
                out->max_y_after_breakout = (y > out->max_y_after_breakout) ? y : out->max_y_after_breakout;
            }
        }
        if (out->detect_step == 0 && gas_at_or_below_exit(&s) != 0) {
            out->detect_step = step;
        }
    }
}

static void
print_stream_sections(void) {
    static uint32_t low[256];
    static uint32_t high[256];

    printf("== raw stream, %d draws ==\n", STREAM_DRAWS);
    printf("%4s  %10s %10s %10s  %10s %10s %10s\n", "seed", "low-min", "low-max", "low-chi2", "high-min", "high-max",
           "high-chi2");
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        byte_histograms(seed, low, high);
        uint32_t lo_min = 0xFFFFFFFFu;
        uint32_t lo_max = 0;
        uint32_t hi_min = 0xFFFFFFFFu;
        uint32_t hi_max = 0;
        for (int i = 0; i < 256; i++) {
            lo_min = (low[i] < lo_min) ? low[i] : lo_min;
            lo_max = (low[i] > lo_max) ? low[i] : lo_max;
            hi_min = (high[i] < hi_min) ? high[i] : hi_min;
            hi_max = (high[i] > hi_max) ? high[i] : hi_max;
        }
        printf("%4u  %10u %10u %10.1f  %10u %10u %10.1f\n", seed, lo_min, lo_max,
               chi_square_flat(low, 256, (double)STREAM_DRAWS), hi_min, hi_max,
               chi_square_flat(high, 256, (double)STREAM_DRAWS));
    }
    printf("255 degrees of freedom: a flat byte sits near 255, p=0.001 at 330.5\n\n");

    printf("== low-byte period, searched to %d over a %d-draw window ==\n", PERIOD_LIMIT, PERIOD_WINDOW);
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        const int p = low_byte_period(seed);
        printf("  seed %2u: %s\n", seed, (p == 0) ? "none" : (p < 0) ? "out of memory" : "REPEATS");
    }
    printf("\n");

    printf("== joint distribution, high nibble of each byte, 16x16 bins ==\n");
    printf("%4s  %14s %16s %14s  %18s\n", "seed", "low_n,low_n+1", "high_n,high_n+1", "low_n,high_n",
           "low_n,low_n+2 (step)");
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        printf("%4u  %14.1f %16.1f %14.1f  %18.1f\n", seed, joint_chi_square(seed, 1, 0, 0),
               joint_chi_square(seed, 1, 24, 24), joint_chi_square(seed, 0, 0, 24), joint_chi_square(seed, 2, 0, 0));
    }
    printf("low_n,high_n is the same draw against itself, not a pair - the column is a control\n\n");

    printf("== a lower-diagonal roll, asked of the raw stream ==\n");
    printf("any low byte under %d; the expected rate is %d/256 = %.6f\n", WALK_LOWER_DIAGONALS, WALK_LOWER_DIAGONALS,
           (double)WALK_LOWER_DIAGONALS / 256.0);
    printf("%4s  %16s %16s  %16s %16s\n", "seed", "P every draw", "P|prev passes", "P every 2nd", "P|prev passes");
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        double p1 = 0.0;
        double c1 = 0.0;
        double p2 = 0.0;
        double c2 = 0.0;
        conditional_rates(seed, 1, &p1, &c1);
        conditional_rates(seed, 2, &p2, &c2);
        printf("%4u  %16.6f %16.6f  %16.6f %16.6f\n", seed, p1, c1, p2, c2);
    }
    printf("\n");
}

int
main(void) {
    printf("generator: xorshift32, shifts (13, 17, 5), 32-bit state - launcher/main/util/rng.h\n");
    printf("rng_chance(), rng_below() and sand_rng_chance_at() all read the LOW bits\n\n");

    print_stream_sections();

    printf("== the sealed pocket, stepped through sand_step() ==\n");
    printf("gas at (%d,%d), one open corner at (%d,%d), mobility %d, %d steps, never cut short\n", POCKET_GAS_X,
           POCKET_GAS_Y, POCKET_EXIT_X, POCKET_EXIT_Y, POCKET_MOBILITY, POCKET_STEPS);
    printf("`seen` is the suite's own criterion - a gas cell at y >= %d - and `left` is the\n", POCKET_EXIT_Y);
    printf("step the gas was no longer in the pocket at all\n");
    printf("a corner roll is %d in 256 and mobility passes 255 in 256, so the gas should\n", WALK_EXIT_ROLLS);
    printf("reach the corner after %.1f steps on average\n", 65536.0 / (double)(WALK_EXIT_ROLLS * POCKET_MOBILITY));
    printf("%4s  %8s %8s %10s %12s %12s\n", "seed", "seen", "left", "corner", "rolls@origin", "rows after");
    static pocket_result_t results[POCKET_SEEDS + 1];
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        run_pocket(seed, &results[seed]);
        const pocket_result_t* r = &results[seed];
        char seen[16];
        char left[16];
        char rows[16];
        snprintf(seen, sizeof seen, "%s", "never");
        if (r->detect_step != 0) {
            snprintf(seen, sizeof seen, "%d", r->detect_step);
        }
        snprintf(left, sizeof left, "%s", "never");
        if (r->breakout_step != 0) {
            snprintf(left, sizeof left, "%d", r->breakout_step);
        }
        snprintf(rows, sizeof rows, "%s", "-");
        if (r->max_y_after_breakout >= 0) {
            snprintf(rows, sizeof rows, "%d..%d", r->min_y_after_breakout, r->max_y_after_breakout);
        }
        printf("%4u  %8s %8s %10d %12u %12s%s\n", seed, seen, left, r->exit_hits, r->origin_walks, rows,
               (r->bad_steps != 0) ? "  UNACCOUNTED DRAWS" : "");
    }
    printf("\n");

    /* Pooled across seeds, and in 16 bins rather than 256: a single seed
     * leaves the pocket after a few dozen rolls, which is far too few for
     * a per-seed statistic to say anything. */
    uint32_t pooled[16];
    uint32_t pooled_total = 0;
    memset(pooled, 0, sizeof pooled);
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        for (int v = 0; v < 256; v++) {
            pooled[v >> 4] += results[seed].origin_walk_hist[v];
            pooled_total += results[seed].origin_walk_hist[v];
        }
    }
    printf("pooled over every seed: %u rolls, 16-bin chi2 %.1f (15 degrees of freedom, p=0.001 at 37.7)\n",
           pooled_total, chi_square_flat(pooled, 16, (double)pooled_total));
    printf("\n");

    printf("== direction rolls drawn while the gas sat in the pocket, 16 bins of 16 ==\n");
    printf("%4s %8s", "seed", "rolls");
    for (int b = 0; b < 16; b++) {
        printf(" %5d", b * 16);
    }
    printf("\n");
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        printf("%4u %8u", seed, results[seed].origin_walks);
        uint32_t bins[16];
        memset(bins, 0, sizeof bins);
        for (int v = 0; v < 256; v++) {
            bins[v >> 4] += results[seed].origin_walk_hist[v];
        }
        for (int b = 0; b < 16; b++) {
            printf(" %5u", bins[b]);
        }
        printf("\n");
    }
    printf("\n");

    printf("== the same rolls, values 0..7 ==\n");
    printf("%4s", "seed");
    for (int v = 0; v < 8; v++) {
        printf(" %5d", v);
    }
    printf("\n");
    for (uint32_t seed = 1; seed <= POCKET_SEEDS; seed++) {
        printf("%4u", seed);
        for (int v = 0; v < 8; v++) {
            printf(" %5u", results[seed].origin_walk_hist[v]);
        }
        printf("\n");
    }

    return 0;
}
