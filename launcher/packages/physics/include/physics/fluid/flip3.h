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
} flip3_tuning_t;

#define FLIP3_TUNING_DEFAULT                                                                                           \
    ((flip3_tuning_t){FLIP3_FLIP_RATIO, FLIP3_PRESSURE_ITERS, FLIP3_OVER_RELAXATION, FLIP3_DRIFT_GAIN})

typedef struct {
    flip3_config_t config;
    flip3_tuning_t tuning;
    int nx, ny, nz;     /* grid cells, one solid layer around the box */
    vec3f_t h, inv_h;   /* cell spacing per axis */
    float radius;       /* how close a particle comes to a wall */
    vec3f_t seed_step;  /* the seed lattice's spacing per axis, tiling the box */
    int seed_count;     /* the lattice's points: a full box */
    float rest_density; /* particles a full cell's trilinear splat holds */

    /* The grid: face velocities and their pre-solve copies, the splat weight
     * one component at a time, a cell's particle density and its kind.
     * Indexed x * ny * nz + y * nz + z. */
    float *u, *v, *w;
    float *prev_u, *prev_v, *prev_w;
    float* weight;
    float* density;
    uint8_t* kind;

    /* The particles, in grid coordinates (the box shifted by one cell). */
    vec3f_t* pos;
    vec3f_t* vel;
    int count;
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

/* The floats flip3_field() writes at `samples` per cell along each axis. */
size_t flip3_field_count(const flip3_t* f, int samples);

/* The surface field: the water's density over the box, 1 at rest, with one
 * empty sample all round so a mesh of it closes. One [1 2 1] / 4 pass per
 * axis rounds it and sinks a film a sample thin; the walls mirror, so water
 * against glass keeps its density. Returns its sizes, origin and spacing in
 * box coordinates, for iso_surface_nets(). */
iso_field_t flip3_field(const flip3_t* f, int samples, float* out);
