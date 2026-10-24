#include "math/motion/flip3.h"

#include <math.h>
#include <string.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

enum { KIND_SOLID, KIND_AIR, KIND_FLUID };

/* Bisection steps that find the fill's level; far past float precision. */
#define FILL_LEVEL_STEPS 48

/* Each array of a block starts at this alignment. */
#define BLOCK_ALIGN      _Alignof(max_align_t)

/* Eight float arrays and the kinds. */
#define GRID_FLOATS      8

static size_t
align_up(size_t n) {
    return (n + BLOCK_ALIGN - 1) / BLOCK_ALIGN * BLOCK_ALIGN;
}

static int
mini(int a, int b) {
    return a < b ? a : b;
}

static int
maxi(int a, int b) {
    return a > b ? a : b;
}

static float
clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static float
axis_of(vec3f_t v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

static void
set_axis(vec3f_t* v, int axis, float value) {
    if (axis == 0) {
        v->x = value;
    } else if (axis == 1) {
        v->y = value;
    } else {
        v->z = value;
    }
}

/* Whole cells inside the box along one axis. */
static int
inner_cells(float size, float cell) {
    return maxi(1, (int)lroundf(size / cell));
}

static bool
config_ok(const flip3_config_t* c) {
    return c->size.x > 0.0F && c->size.y > 0.0F && c->size.z > 0.0F && c->cell > 0.0F && c->max_particles > 0;
}

/* Grid cells per axis, walls included. */
static int
grid_along(const flip3_config_t* c, int axis) {
    return inner_cells(axis_of(c->size, axis), c->cell) + 2;
}

size_t
flip3_grid_bytes(const flip3_config_t* c) {
    if (!config_ok(c)) {
        return 0;
    }
    const size_t n = (size_t)grid_along(c, 0) * (size_t)grid_along(c, 1) * (size_t)grid_along(c, 2);
    return (GRID_FLOATS * align_up(n * sizeof(float))) + align_up(n);
}

size_t
flip3_particle_bytes(const flip3_config_t* c) {
    if (!config_ok(c)) {
        return 0;
    }
    return 2 * align_up((size_t)c->max_particles * sizeof(vec3f_t));
}

static void*
carve(uint8_t** at, size_t bytes) {
    void* p = *at;
    *at += align_up(bytes);
    return p;
}

bool
flip3_init(flip3_t* f, const flip3_config_t* c, void* grid, void* particles) {
    memset(f, 0, sizeof *f);
    if (!config_ok(c) || grid == NULL || particles == NULL) {
        return false;
    }
    f->config = *c;
    f->tuning = FLIP3_TUNING_DEFAULT;
    f->radius = FLIP3_RADIUS_CELLS * c->cell;
    const float spacing = FLIP3_SEED_SPACING_CELLS * c->cell;
    int* cells[3] = {&f->nx, &f->ny, &f->nz};
    f->seed_count = 1;
    f->rest_density = 1.0F;
    for (int a = 0; a < 3; a++) {
        /* The seed lattice tiles the box: whole steps near the seed spacing. */
        const float size = axis_of(c->size, a);
        *cells[a] = grid_along(c, a);
        const float h = size / (float)(*cells[a] - 2);
        const int seeds = maxi(1, (int)lroundf(size / spacing));
        const float step = size / (float)seeds;
        set_axis(&f->h, a, h);
        set_axis(&f->inv_h, a, 1.0F / h);
        set_axis(&f->seed_step, a, step);
        f->seed_count *= seeds;
        f->rest_density *= h / step;
    }

    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    uint8_t* g = grid;
    float** floats[GRID_FLOATS] = {&f->u, &f->v, &f->w, &f->prev_u, &f->prev_v, &f->prev_w, &f->weight, &f->density};
    for (int i = 0; i < GRID_FLOATS; i++) {
        *floats[i] = carve(&g, n * sizeof(float));
        memset(*floats[i], 0, n * sizeof(float));
    }
    f->kind = carve(&g, n);

    const size_t m = (size_t)c->max_particles;
    uint8_t* p = particles;
    f->pos = carve(&p, m * sizeof(vec3f_t));
    f->vel = carve(&p, m * sizeof(vec3f_t));
    return true;
}

static int
cell_index(const flip3_t* f, int x, int y, int z) {
    return (((x * f->ny) + y) * f->nz) + z;
}

static void update_density(flip3_t* f);

/* The seed lattice, each point centred in its step. */
typedef struct {
    int n[3];
    vec3f_t step, first;
} lattice_t;

static lattice_t
fill_lattice(const flip3_t* f) {
    lattice_t l = {.step = f->seed_step};
    for (int a = 0; a < 3; a++) {
        const float step = axis_of(f->seed_step, a);
        l.n[a] = (int)lroundf(axis_of(f->config.size, a) / step);
        set_axis(&l.first, a, axis_of(f->h, a) + fmaxf(0.5F * step, f->radius));
    }
    return l;
}

static vec3f_t
lattice_point(const lattice_t* l, int i, int j, int k) {
    return vec3f_add(l->first, (vec3f_t){(float)i * l->step.x, (float)j * l->step.y, (float)k * l->step.z});
}

/* Lattice points at or below `level`, where below is further along down. */
static int
count_below(const lattice_t* l, vec3f_t down, float level) {
    int count = 0;
    for (int i = 0; i < l->n[0]; i++) {
        for (int j = 0; j < l->n[1]; j++) {
            for (int k = 0; k < l->n[2]; k++) {
                count += vec3f_dot(lattice_point(l, i, j, k), down) >= level ? 1 : 0;
            }
        }
    }
    return count;
}

int
flip3_fill(flip3_t* f, float fraction, vec3f_t down) {
    const lattice_t l = fill_lattice(f);
    const int wanted = (int)lroundf(clampf(fraction, 0.0F, 1.0F) * (float)f->seed_count);
    const int target = mini(wanted, f->config.max_particles);

    /* The level with `target` points below it, between the keys of the
     * lattice's top and bottom corners. */
    float lo = INFINITY;
    float hi = -INFINITY;
    for (int corner = 0; corner < 8; corner++) {
        const vec3f_t p = lattice_point(&l, (corner & 1) != 0 ? l.n[0] - 1 : 0, (corner & 2) != 0 ? l.n[1] - 1 : 0,
                                        (corner & 4) != 0 ? l.n[2] - 1 : 0);
        const float key = vec3f_dot(p, down);
        lo = fminf(lo, key);
        hi = fmaxf(hi, key);
    }
    for (int step = 0; step < FILL_LEVEL_STEPS; step++) {
        const float mid = 0.5F * (lo + hi);
        if (count_below(&l, down, mid) >= target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    f->count = 0;
    for (int i = 0; i < l.n[0] && f->count < target; i++) {
        for (int j = 0; j < l.n[1] && f->count < target; j++) {
            for (int k = 0; k < l.n[2] && f->count < target; k++) {
                const vec3f_t p = lattice_point(&l, i, j, k);
                if (vec3f_dot(p, down) >= lo) {
                    f->pos[f->count] = p;
                    f->vel[f->count] = (vec3f_t){0};
                    f->count++;
                }
            }
        }
    }
    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    memset(f->u, 0, n * sizeof(float));
    memset(f->v, 0, n * sizeof(float));
    memset(f->w, 0, n * sizeof(float));
    update_density(f);
    return f->count;
}

static void
advect(flip3_t* f, float dt) {
    for (int i = 0; i < f->count; i++) {
        f->pos[i] = vec3f_add(f->pos[i], vec3f_scale(f->vel[i], dt));
    }
}

/* The walls: a particle stays a radius inside the box, and loses the
 * velocity that carried it out. */
static void
collide(flip3_t* f) {
    for (int a = 0; a < 3; a++) {
        const float lo = axis_of(f->h, a) + f->radius;
        const float hi = axis_of(f->h, a) + axis_of(f->config.size, a) - f->radius;
        for (int i = 0; i < f->count; i++) {
            const float p = axis_of(f->pos[i], a);
            if (p < lo || p > hi) {
                set_axis(&f->pos[i], a, p < lo ? lo : hi);
                set_axis(&f->vel[i], a, 0.0F);
            }
        }
    }
}

/* A point's eight grid neighbours for samples offset by `o` from the cell
 * corner, and their trilinear weights. */
typedef struct {
    int at[8];
    float weight[8];
} stencil_t;

/* Both walls' faces must be reachable: Muller caps the upper neighbour one
 * short, which his tank hides by keeping its floor at index 0. */
static void
stencil_axis(float p, float o, float h, float inv_h, int n, int* i0, int* i1, float* t) {
    const float q = clampf(p, h, (float)(n - 1) * h) - o;
    *i0 = mini((int)floorf(q * inv_h), n - 2);
    *t = (q - ((float)*i0 * h)) * inv_h;
    *i1 = *i0 + 1;
}

/* Cell centres inside the box only: weight that would land in a wall stays
 * in the cell beside it, so a full cell beside a wall reads rest density. */
static void
centre_axis(float p, float h, float inv_h, int n, int* i0, int* i1, float* t) {
    const float q = p - (0.5F * h);
    *i0 = mini(maxi((int)floorf(q * inv_h), 1), n - 2);
    *t = clampf((q - ((float)*i0 * h)) * inv_h, 0.0F, 1.0F);
    *i1 = mini(*i0 + 1, n - 2);
}

static stencil_t
corners(const flip3_t* f, int x0, int x1, int y0, int y1, int z0, int z1, float tx, float ty, float tz) {
    stencil_t s;
    for (int corner = 0; corner < 8; corner++) {
        const bool bx = (corner & 1) != 0;
        const bool by = (corner & 2) != 0;
        const bool bz = (corner & 4) != 0;
        s.at[corner] = cell_index(f, bx ? x1 : x0, by ? y1 : y0, bz ? z1 : z0);
        s.weight[corner] = (bx ? tx : 1.0F - tx) * (by ? ty : 1.0F - ty) * (bz ? tz : 1.0F - tz);
    }
    return s;
}

static stencil_t
stencil(const flip3_t* f, vec3f_t p, vec3f_t o) {
    int x0, x1, y0, y1, z0, z1;
    float tx, ty, tz;
    stencil_axis(p.x, o.x, f->h.x, f->inv_h.x, f->nx, &x0, &x1, &tx);
    stencil_axis(p.y, o.y, f->h.y, f->inv_h.y, f->ny, &y0, &y1, &ty);
    stencil_axis(p.z, o.z, f->h.z, f->inv_h.z, f->nz, &z0, &z1, &tz);
    return corners(f, x0, x1, y0, y1, z0, z1, tx, ty, tz);
}

static stencil_t
centre_stencil(const flip3_t* f, vec3f_t p) {
    int x0, x1, y0, y1, z0, z1;
    float tx, ty, tz;
    centre_axis(p.x, f->h.x, f->inv_h.x, f->nx, &x0, &x1, &tx);
    centre_axis(p.y, f->h.y, f->inv_h.y, f->ny, &y0, &y1, &ty);
    centre_axis(p.z, f->h.z, f->inv_h.z, f->nz, &z0, &z1, &tz);
    return corners(f, x0, x1, y0, y1, z0, z1, tx, ty, tz);
}

static int
cell_of(float p, float inv_h, int n) {
    return mini(maxi((int)floorf(p * inv_h), 0), n - 1);
}

static void
mark_kinds(flip3_t* f) {
    for (int x = 0; x < f->nx; x++) {
        for (int y = 0; y < f->ny; y++) {
            for (int z = 0; z < f->nz; z++) {
                const bool wall = x == 0 || y == 0 || z == 0 || x == f->nx - 1 || y == f->ny - 1 || z == f->nz - 1;
                f->kind[cell_index(f, x, y, z)] = wall ? KIND_SOLID : KIND_AIR;
            }
        }
    }
    for (int i = 0; i < f->count; i++) {
        const vec3f_t p = f->pos[i];
        uint8_t* k = &f->kind[cell_index(f, cell_of(p.x, f->inv_h.x, f->nx), cell_of(p.y, f->inv_h.y, f->ny),
                                         cell_of(p.z, f->inv_h.z, f->nz))];
        if (*k == KIND_AIR) {
            *k = KIND_FLUID;
        }
    }
}

typedef struct {
    float *field, *prev;
    vec3f_t offset;
    int stride; /* to the cell on the face's other side */
} component_t;

static void
components(flip3_t* f, component_t out[3]) {
    const vec3f_t half = vec3f_scale(f->h, 0.5F);
    out[0] = (component_t){f->u, f->prev_u, {0.0F, half.y, half.z}, f->ny * f->nz};
    out[1] = (component_t){f->v, f->prev_v, {half.x, 0.0F, half.z}, f->nz};
    out[2] = (component_t){f->w, f->prev_w, {half.x, half.y, 0.0F}, 1};
}

static void
to_grid(flip3_t* f) {
    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    mark_kinds(f);
    component_t comps[3];
    components(f, comps);
    for (int a = 0; a < 3; a++) {
        const component_t* c = &comps[a];
        memset(c->field, 0, n * sizeof(float));
        memset(f->weight, 0, n * sizeof(float));
        for (int i = 0; i < f->count; i++) {
            const stencil_t s = stencil(f, f->pos[i], c->offset);
            const float pv = axis_of(f->vel[i], a);
            for (int k = 0; k < 8; k++) {
                c->field[s.at[k]] += pv * s.weight[k];
                f->weight[s.at[k]] += s.weight[k];
            }
        }
        for (size_t i = 0; i < n; i++) {
            if (f->weight[i] > 0.0F) {
                c->field[i] /= f->weight[i];
            }
            /* Static walls: a face of a solid cell carries nothing. */
            const bool solid_side = i >= (size_t)c->stride && f->kind[i - (size_t)c->stride] == KIND_SOLID;
            if (f->kind[i] == KIND_SOLID || solid_side) {
                c->field[i] = 0.0F;
            }
        }
    }
}

static void
update_density(flip3_t* f) {
    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    memset(f->density, 0, n * sizeof(float));
    for (int i = 0; i < f->count; i++) {
        const stencil_t s = centre_stencil(f, f->pos[i]);
        for (int k = 0; k < 8; k++) {
            f->density[s.at[k]] += s.weight[k];
        }
    }
}

/* Gauss-Seidel on the divergence over cells of any aspect, with Muller's
 * drift push: a cell packed past rest density gets that share of its excess
 * as outflow per second over dt. */
/* The particles' velocities, kept for FLIP's change, then the acceleration
 * on every face that is not a wall's. */
static void
accelerate(flip3_t* f, float dt, vec3f_t accel) {
    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    component_t comps[3];
    components(f, comps);
    for (int a = 0; a < 3; a++) {
        const component_t* c = &comps[a];
        memcpy(c->prev, c->field, n * sizeof(float));
        const float dv = axis_of(accel, a) * dt;
        for (size_t i = (size_t)c->stride; i < n; i++) {
            if (f->kind[i] != KIND_SOLID && f->kind[i - (size_t)c->stride] != KIND_SOLID) {
                c->field[i] += dv;
            }
        }
    }
}

static void
solve_pressure(flip3_t* f, float dt) {
    const int sx = f->ny * f->nz;
    const int sy = f->nz;
    const vec3f_t ih = f->inv_h;
    const vec3f_t ih2 = {ih.x * ih.x, ih.y * ih.y, ih.z * ih.z};
    const float drift = f->tuning.drift_gain / dt / f->rest_density;
    for (int iter = 0; iter < f->tuning.pressure_iters; iter++) {
        for (int x = 1; x < f->nx - 1; x++) {
            for (int y = 1; y < f->ny - 1; y++) {
                for (int z = 1; z < f->nz - 1; z++) {
                    const int c = cell_index(f, x, y, z);
                    if (f->kind[c] != KIND_FLUID) {
                        continue;
                    }
                    const float s0x = f->kind[c - sx] != KIND_SOLID ? 1.0F : 0.0F;
                    const float s1x = f->kind[c + sx] != KIND_SOLID ? 1.0F : 0.0F;
                    const float s0y = f->kind[c - sy] != KIND_SOLID ? 1.0F : 0.0F;
                    const float s1y = f->kind[c + sy] != KIND_SOLID ? 1.0F : 0.0F;
                    const float s0z = f->kind[c - 1] != KIND_SOLID ? 1.0F : 0.0F;
                    const float s1z = f->kind[c + 1] != KIND_SOLID ? 1.0F : 0.0F;
                    const float s = ((s0x + s1x) * ih2.x) + ((s0y + s1y) * ih2.y) + ((s0z + s1z) * ih2.z);
                    if (s == 0.0F) {
                        continue;
                    }
                    float div = ((f->u[c + sx] - f->u[c]) * ih.x) + ((f->v[c + sy] - f->v[c]) * ih.y)
                                + ((f->w[c + 1] - f->w[c]) * ih.z);
                    /* Muller pushes only packed cells apart. A divergence-free
                     * field never closes a gap either, so cells with water all
                     * round are also drawn back to rest; at the surface that
                     * pull would act as a false surface tension. */
                    const bool enclosed = f->kind[c - sx] != KIND_AIR && f->kind[c + sx] != KIND_AIR
                                          && f->kind[c - sy] != KIND_AIR && f->kind[c + sy] != KIND_AIR
                                          && f->kind[c - 1] != KIND_AIR && f->kind[c + 1] != KIND_AIR;
                    const float compression = f->density[c] - f->rest_density;
                    if (compression > 0.0F || enclosed) {
                        div -= drift * compression;
                    }
                    const float p = -div / s * f->tuning.over_relaxation;
                    f->u[c] -= s0x * p * ih.x;
                    f->u[c + sx] += s1x * p * ih.x;
                    f->v[c] -= s0y * p * ih.y;
                    f->v[c + sy] += s1y * p * ih.y;
                    f->w[c] -= s0z * p * ih.z;
                    f->w[c + 1] += s1z * p * ih.z;
                }
            }
        }
    }
}

static void
to_particles(flip3_t* f) {
    component_t comps[3];
    components(f, comps);
    const float flip = f->tuning.flip_ratio;
    for (int a = 0; a < 3; a++) {
        const component_t* c = &comps[a];
        for (int i = 0; i < f->count; i++) {
            const stencil_t s = stencil(f, f->pos[i], c->offset);
            float total = 0.0F;
            float pic = 0.0F;
            float change = 0.0F;
            for (int k = 0; k < 8; k++) {
                const int at = s.at[k];
                if (f->kind[at] == KIND_AIR && f->kind[at - c->stride] == KIND_AIR) {
                    continue;
                }
                total += s.weight[k];
                pic += s.weight[k] * c->field[at];
                change += s.weight[k] * (c->field[at] - c->prev[at]);
            }
            if (total > 0.0F) {
                const float old = axis_of(f->vel[i], a);
                set_axis(&f->vel[i], a, ((1.0F - flip) * (pic / total)) + (flip * (old + (change / total))));
            }
        }
    }
}

void
flip3_step(flip3_t* f, float dt_s, vec3f_t accel) {
    if (dt_s <= 0.0F || f->count == 0) {
        return;
    }
    to_grid(f);
    update_density(f);
    accelerate(f, dt_s, accel);
    solve_pressure(f, dt_s);
    to_particles(f);
    advect(f, dt_s);
    collide(f);
}

bool
flip3_density_field(const flip3_t* f, int samples, float* field, int sx, int sy, int sz) {
    if (samples < 1 || sx != (flip3_cells_x(f) * samples) + 2 || sy != (flip3_cells_y(f) * samples) + 2
        || sz != (flip3_cells_z(f) * samples) + 2) {
        return false;
    }
    const vec3f_t step = vec3f_scale(f->h, 1.0F / (float)samples);
    const float inv_rest = 1.0F / f->rest_density;
    for (int x = 0; x < sx; x++) {
        for (int y = 0; y < sy; y++) {
            for (int z = 0; z < sz; z++) {
                float* out = &field[(((x * sy) + y) * sz) + z];
                if (x == 0 || y == 0 || z == 0 || x == sx - 1 || y == sy - 1 || z == sz - 1) {
                    *out = 0.0F;
                    continue;
                }
                /* Grid coordinates: the box starts one cell in. */
                const vec3f_t p = {f->h.x + (((float)x - 0.5F) * step.x), f->h.y + (((float)y - 0.5F) * step.y),
                                   f->h.z + (((float)z - 0.5F) * step.z)};
                const stencil_t s = centre_stencil(f, p);
                float d = 0.0F;
                for (int k = 0; k < 8; k++) {
                    d += s.weight[k] * f->density[s.at[k]];
                }
                *out = d * inv_rest;
            }
        }
    }
    return true;
}
