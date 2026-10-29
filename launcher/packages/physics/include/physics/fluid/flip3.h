/*
 * flip3: water in a closed box, simulated as FLIP particles on a MAC grid
 * (Harlow and Welch; Brackbill and Ruppel), blended with PIC as in Bridson,
 * with the drift push of Muller's Ten Minute Physics FLIP tank, in 3-D.
 *
 * The box is axis aligned, from the origin to `size`, in any length unit;
 * the acceleration given to a step is in that unit per second squared and
 * acts on the water as gravity does, so a box that is being shaken passes
 * gravity minus its own acceleration. Nothing here knows what the axes mean.
 *
 * A step is Bridson's order: particles to grid, acceleration, pressure,
 * grid to particles, then the particles move with the projected velocity.
 * Muller's tank moves them first and pushes overlapping pairs apart; here
 * that sank a resting pool by accel * dt^2 a step, and the push-apart held
 * heaps up as a granular pile does, so neither is done. Density is held by
 * the drift push alone, both ways in cells with water all round.
 *
 * The caller owns the memory, in two blocks so each can be placed apart:
 * the grid, which every pass walks, and the particles. flip3 allocates
 * nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/geometry/iso_surface.h"
#include "math/linear/vec3f.h"

/* How close a particle may come to a wall, and the seed lattice's spacing,
 * as fractions of a cell: Muller's radius 0.3, and seeds a diameter apart. */
#define FLIP3_RADIUS_CELLS       0.3F
#define FLIP3_SEED_SPACING_CELLS (2.0F * FLIP3_RADIUS_CELLS)

/* Muller's defaults: the share of FLIP in the grid-to-particle blend, and
 * the pressure solve's sweeps and over-relaxation. */
#define FLIP3_FLIP_RATIO         0.9F
#define FLIP3_PRESSURE_ITERS     30
#define FLIP3_OVER_RELAXATION    1.9F

/* The drift push: a cell off rest density gets this share of the
 * difference as flow per step, so it means the same at any length unit.
 * Ours, the smallest that holds a pool's volume over a minute. */
#define FLIP3_DRIFT_GAIN         0.1F

typedef struct {
    vec3f_t size; /* the box's inside */
    float cell;   /* the grid's spacing, rounded per axis so whole cells fill the box */
    int max_particles;
} flip3_config_t;

typedef struct {
    float flip_ratio; /* 0 is PIC, viscous and calm; 1 is FLIP, lively and noisy */
    int pressure_iters;
    float over_relaxation;
    float drift_gain;
    /* Relax the cells in two checkerboard colours, each colour's cells in
     * two halves that flip3_split_with() may run at once (red-black
     * Gauss-Seidel), rather than in index order on one core. */
    bool red_black;
} flip3_tuning_t;

#define FLIP3_TUNING_DEFAULT                                                                                           \
    ((flip3_tuning_t){FLIP3_FLIP_RATIO, FLIP3_PRESSURE_ITERS, FLIP3_OVER_RELAXATION, FLIP3_DRIFT_GAIN, false})

/* The step's stages, in the order they run, with the names a timing log
 * gives them. */
// clang-format off
#define FLIP3_STAGES(X) \
    X(P2G, "p2g") X(ACCEL, "accel") X(PRESSURE, "pressure") X(G2P, "g2p") \
    X(ADVECT, "advect") X(COLLIDE, "collide")
// clang-format on

#define FLIP3_STAGE_ENUM(id, name) FLIP3_STAGE_##id,

typedef enum { FLIP3_STAGES(FLIP3_STAGE_ENUM) FLIP3_STAGE_COUNT } flip3_stage_t;

extern const char* const flip3_stage_names[FLIP3_STAGE_COUNT];

/* A microsecond clock, for timing the step's stages. */
typedef int64_t (*flip3_clock_fn)(void);

/* The places the step and flip3_field() split their work, so a host can
 * keep a cut for each: flip3_field()'s sampling, then its smoothing along
 * x, y and z. */
enum {
    FLIP3_SPLIT_SPLAT,
    FLIP3_SPLIT_GATHER,
    FLIP3_SPLIT_RED_BLACK,
    FLIP3_SPLIT_FIELD_SAMPLE,
    FLIP3_SPLIT_FIELD_SMOOTH_X,
    FLIP3_SPLIT_FIELD_SMOOTH_Y,
    FLIP3_SPLIT_FIELD_SMOOTH_Z,
    FLIP3_SPLIT_COUNT
};

/* A stage's work over items [from, to). Two ranges never write the same
 * memory, so they may run at once. */
typedef void (*flip3_range_fn)(void* ctx, int from, int to);

/* Runs fn over items [0, count) as two ranges, cut anywhere, at once on two
 * cores or in turn, and returns when both are done. `site` is one of the
 * FLIP3_SPLIT_ places. */
typedef void (*flip3_split_fn)(int site, flip3_range_fn fn, void* ctx, int count);

typedef struct {
    flip3_config_t config;
    flip3_tuning_t tuning;
    int nx, ny, nz;     /* grid cells, one solid layer around the box */
    vec3f_t h, inv_h;   /* cell spacing per axis */
    float radius;       /* how close a particle comes to a wall */
    vec3f_t seed_step;  /* the seed lattice's spacing per axis, tiling the box */
    int seed_count;     /* the lattice's points: a full box */
    float rest_density; /* particles a full cell's trilinear splat holds */

    /* The grid: face velocities and their pre-solve copies (which hold the
     * splat's weights until then), a cell's particle density and its kind.
     * Indexed x * ny * nz + y * nz + z. */
    float *u, *v, *w;
    float *prev_u, *prev_v, *prev_w;
    float* density;
    uint8_t* kind;
    uint8_t* sides; /* a water cell's open sides, for the pressure solve */

    /* The particles, in grid coordinates (the box shifted by one cell). */
    vec3f_t* pos;
    vec3f_t* vel;
    int count;

    /* Set by flip3_time_stages(); NULL times nothing. */
    flip3_clock_fn clock;
    int64_t* stage_us;

    /* Set by flip3_split_with(); NULL runs both halves in turn. */
    flip3_split_fn split;
} flip3_t;

/* The two blocks `c` needs, each at malloc alignment; 0 for a configuration
 * flip3_init() refuses. */
size_t flip3_grid_bytes(const flip3_config_t* c);
size_t flip3_particle_bytes(const flip3_config_t* c);

/* False when a block is NULL or the configuration cannot be met. Starts
 * empty, at the default tuning. */
bool flip3_init(flip3_t* f, const flip3_config_t* c, void* grid, void* particles);

/* Empties the box and refills `fraction` (0 to 1) of its volume at rest,
 * on a lattice that tiles the box, lowest first along `down` (need not be
 * unit). Returns the particles placed, capped by max_particles. */
int flip3_fill(flip3_t* f, float fraction, vec3f_t down);

/* Advances `dt_s` seconds under `accel`, one substep. */
void flip3_step(flip3_t* f, float dt_s, vec3f_t accel);

/* From now on each step adds its stages' microseconds by `clock` to
 * `stage_us`, FLIP3_STAGE_COUNT of them; a NULL clock stops. flip3_init()
 * stops it too. */
static inline void
flip3_time_stages(flip3_t* f, flip3_clock_fn clock, int64_t* stage_us) {
    f->clock = stage_us != NULL ? clock : NULL;
    f->stage_us = stage_us;
}

/* From now on the step's particle passes and flip3_field() hand their work
 * to `split`; the results are the same as on one core, wherever it cuts.
 * NULL stops, and so does flip3_init(). */
static inline void
flip3_split_with(flip3_t* f, flip3_split_fn split) {
    f->split = split;
}

/* The fastest particle's speed squared: 0 for a box at rest. */
float flip3_peak_speed_sq(const flip3_t* f);

/* The share of the box's volume the particles stand for. */
static inline float
flip3_fraction(const flip3_t* f) {
    return (float)f->count / (float)f->seed_count;
}

/* Particle `i` in box coordinates. */
static inline vec3f_t
flip3_particle(const flip3_t* f, int i) {
    return vec3f_sub(f->pos[i], f->h);
}

/* The floats flip3_field() writes at `samples` per cell along each axis.
 * `samples` may be fractional: each axis takes its cells times `samples`,
 * rounded, at least one, spaced to fill the box. */
size_t flip3_field_count(const flip3_t* f, float samples);

/* The surface field: the water's density over the box, 1 at rest. One
 * [1 2 1] / 4 pass per axis rounds it and sinks a film a sample thin; the
 * walls mirror, so water against glass keeps its density. Returns its sizes,
 * origin and spacing in box coordinates, for iso_surface_nets(). */
iso_field_t flip3_field(const flip3_t* f, float samples, float* out);
