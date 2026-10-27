#include "physics/fluid/flip3.h"

#include <math.h>

#include <string.h>
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

enum { KIND_SOLID, KIND_AIR, KIND_FLUID };

/* Bisection steps that find the fill's level: a float's 24 bits. */
#define FILL_LEVEL_STEPS           24

/* Each array of a block starts at this alignment. */
#define BLOCK_ALIGN                _Alignof(max_align_t)

/* Seven float arrays and the kinds. */
#define GRID_FLOATS                7

#define FLIP3_STAGE_NAME(id, name) name,
const char* const flip3_stage_names[FLIP3_STAGE_COUNT] = {FLIP3_STAGES(FLIP3_STAGE_NAME)};

static size_t
align_up(size_t n) {
    return mathi_size_ceil(n, BLOCK_ALIGN);
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
    return mathi_max(1, (int)lroundf(size / cell));
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
        const int seeds = mathi_max(1, (int)lroundf(size / spacing));
        const float step = size / (float)seeds;
        set_axis(&f->h, a, h);
        set_axis(&f->inv_h, a, 1.0F / h);
        set_axis(&f->seed_step, a, step);
        f->seed_count *= seeds;
        f->rest_density *= h / step;
    }

    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    uint8_t* g = grid;
    float** floats[GRID_FLOATS] = {&f->u, &f->v, &f->w, &f->prev_u, &f->prev_v, &f->prev_w, &f->density};
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

/* The level with `target` lattice points below it, bisected between the
 * keys of the lattice's top and bottom corners. */
static float
fill_level(const lattice_t* l, vec3f_t down, int target) {
    float lo = INFINITY;
    float hi = -INFINITY;
    for (int corner = 0; corner < 8; corner++) {
        const vec3f_t p = lattice_point(l, (corner & 1) != 0 ? l->n[0] - 1 : 0, (corner & 2) != 0 ? l->n[1] - 1 : 0,
                                        (corner & 4) != 0 ? l->n[2] - 1 : 0);
        const float key = vec3f_dot(p, down);
        lo = fminf(lo, key);
        hi = fmaxf(hi, key);
    }
    for (int step = 0; step < FILL_LEVEL_STEPS; step++) {
        const float mid = 0.5F * (lo + hi);
        if (count_below(l, down, mid) >= target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Up to `target` particles at rest on the lattice points at or below `level`. */
static void
place_below(flip3_t* f, const lattice_t* l, vec3f_t down, float level, int target) {
    f->count = 0;
    const int points = l->n[0] * l->n[1] * l->n[2];
    for (int i = 0; i < points && f->count < target; i++) {
        const vec3f_t p = lattice_point(l, i / (l->n[1] * l->n[2]), (i / l->n[2]) % l->n[1], i % l->n[2]);
        if (vec3f_dot(p, down) >= level) {
            f->pos[f->count] = p;
            f->vel[f->count] = (vec3f_t){0};
            f->count++;
        }
    }
}

int
flip3_fill(flip3_t* f, float fraction, vec3f_t down) {
    const lattice_t l = fill_lattice(f);
    const int wanted = (int)lroundf(mathf_clamp(fraction, 0.0F, 1.0F) * (float)f->seed_count);
    const int target = mathi_min(wanted, f->config.max_particles);
    place_below(f, &l, down, fill_level(&l, down, target), target);
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
    const float q = mathf_clamp(p, h, (float)(n - 1) * h) - o;
    *i0 = mathi_min((int)floorf(q * inv_h), n - 2);
    *t = (q - ((float)*i0 * h)) * inv_h;
    *i1 = *i0 + 1;
}

/* Cell centres inside the box only: weight that would land in a wall stays
 * in the cell beside it, so a full cell beside a wall reads rest density. */
static void
centre_axis(float p, float h, float inv_h, int n, int* i0, int* i1, float* t) {
    const float q = p - (0.5F * h);
    *i0 = mathi_min(mathi_max((int)floorf(q * inv_h), 1), n - 2);
    *t = mathf_clamp((q - ((float)*i0 * h)) * inv_h, 0.0F, 1.0F);
    *i1 = mathi_min(*i0 + 1, n - 2);
}

static stencil_t
corners(const flip3_t* f, const int lo[3], const int hi[3], const float t[3]) {
    stencil_t s;
    for (int corner = 0; corner < 8; corner++) {
        const bool bx = (corner & 1) != 0;
        const bool by = (corner & 2) != 0;
        const bool bz = (corner & 4) != 0;
        s.at[corner] = cell_index(f, bx ? hi[0] : lo[0], by ? hi[1] : lo[1], bz ? hi[2] : lo[2]);
        s.weight[corner] = (bx ? t[0] : 1.0F - t[0]) * (by ? t[1] : 1.0F - t[1]) * (bz ? t[2] : 1.0F - t[2]);
    }
    return s;
}

/* Grid cells per axis, walls included. */
static int
cells_on(const flip3_t* f, int axis) {
    return axis == 0 ? f->nx : (axis == 1 ? f->ny : f->nz);
}

static stencil_t
centre_stencil(const flip3_t* f, vec3f_t p) {
    int lo[3];
    int hi[3];
    float t[3];
    for (int a = 0; a < 3; a++) {
        centre_axis(axis_of(p, a), axis_of(f->h, a), axis_of(f->inv_h, a), cells_on(f, a), &lo[a], &hi[a], &t[a]);
    }
    return corners(f, lo, hi, t);
}

/* A particle's place per axis for the stencils that need it: at the faces
 * (no offset along the axis) and half a cell in, each the lower cell and
 * the fraction toward the next, worked out once per particle. */
enum { AT_FACE, AT_HALF, AT_KINDS };

typedef struct {
    int lo[AT_KINDS][3];
    float t[AT_KINDS][3];
} place_t;

static place_t
place_of(const flip3_t* f, vec3f_t p) {
    place_t out;
    for (int a = 0; a < 3; a++) {
        const float h = axis_of(f->h, a);
        const float inv_h = axis_of(f->inv_h, a);
        int hi;
        stencil_axis(axis_of(p, a), 0.0F, h, inv_h, cells_on(f, a), &out.lo[AT_FACE][a], &hi, &out.t[AT_FACE][a]);
        stencil_axis(axis_of(p, a), 0.5F * h, h, inv_h, cells_on(f, a), &out.lo[AT_HALF][a], &hi, &out.t[AT_HALF][a]);
    }
    return out;
}

/* Velocity component `axis`'s stencil: on faces along it, half a cell in
 * across it (a MAC grid's face centres). */
static stencil_t
face_stencil(const flip3_t* f, const place_t* place, int axis) {
    int lo[3];
    int hi[3];
    float t[3];
    for (int a = 0; a < 3; a++) {
        const int at = a == axis ? AT_FACE : AT_HALF;
        lo[a] = place->lo[at][a];
        hi[a] = lo[a] + 1;
        t[a] = place->t[at][a];
    }
    return corners(f, lo, hi, t);
}

static int
cell_of(float p, float inv_h, int n) {
    return mathi_min(mathi_max((int)floorf(p * inv_h), 0), n - 1);
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
    int stride; /* to the cell on the face's other side */
} component_t;

static void
components(flip3_t* f, component_t out[3]) {
    out[0] = (component_t){f->u, f->prev_u, f->ny * f->nz};
    out[1] = (component_t){f->v, f->prev_v, f->nz};
    out[2] = (component_t){f->w, f->prev_w, 1};
}

/* A particle's share of the density at the cell centres. */
static void
add_density(flip3_t* f, vec3f_t p) {
    const stencil_t s = centre_stencil(f, p);
    for (int k = 0; k < 8; k++) {
        f->density[s.at[k]] += s.weight[k];
    }
}

/* Each face's weighted mean, the weights summed in the pre-solve copy;
 * static walls: a face of a solid cell carries nothing. */
static void
settle_faces(const flip3_t* f, const component_t* c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (c->prev[i] > 0.0F) {
            c->field[i] /= c->prev[i];
        }
        const bool solid_side = i >= (size_t)c->stride && f->kind[i - (size_t)c->stride] == KIND_SOLID;
        if (f->kind[i] == KIND_SOLID || solid_side) {
            c->field[i] = 0.0F;
        }
    }
}

/* Every particle in one pass: its velocity splatted onto the faces of all
 * three components, their weights summed in the pre-solve copies (which
 * accelerate() then overwrites), and its density at the cell centres. */
static void
to_grid(flip3_t* f) {
    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    mark_kinds(f);
    component_t comps[3];
    components(f, comps);
    for (int a = 0; a < 3; a++) {
        memset(comps[a].field, 0, n * sizeof(float));
        memset(comps[a].prev, 0, n * sizeof(float));
    }
    memset(f->density, 0, n * sizeof(float));
    for (int i = 0; i < f->count; i++) {
        const place_t place = place_of(f, f->pos[i]);
        for (int a = 0; a < 3; a++) {
            const stencil_t s = face_stencil(f, &place, a);
            const float pv = axis_of(f->vel[i], a);
            for (int k = 0; k < 8; k++) {
                comps[a].field[s.at[k]] += pv * s.weight[k];
                comps[a].prev[s.at[k]] += s.weight[k];
            }
        }
        add_density(f, f->pos[i]);
    }
    for (int a = 0; a < 3; a++) {
        settle_faces(f, &comps[a], n);
    }
}

static void
update_density(flip3_t* f) {
    const size_t n = (size_t)f->nx * (size_t)f->ny * (size_t)f->nz;
    memset(f->density, 0, n * sizeof(float));
    for (int i = 0; i < f->count; i++) {
        add_density(f, f->pos[i]);
    }
}

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

/* What every cell's relaxation shares: neighbour strides, inverse spacing
 * and its square per axis, and the drift push's gain per unit of density
 * error. */
typedef struct {
    int stride[3];
    float ih[3], ih2[3];
    float drift;
} relax_t;

/* 1 for a face that opens onto water or air, 0 for a wall's. */
static float
open_toward(const flip3_t* f, int cell) {
    return f->kind[cell] != KIND_SOLID ? 1.0F : 0.0F;
}

/* Water or wall on all six sides. */
static bool
enclosed(const flip3_t* f, int c, const relax_t* r) {
    for (int a = 0; a < 3; a++) {
        if (f->kind[c - r->stride[a]] == KIND_AIR || f->kind[c + r->stride[a]] == KIND_AIR) {
            return false;
        }
    }
    return true;
}

/* One Gauss-Seidel update of a water cell's six faces. Muller pushes only
 * packed cells apart. A divergence-free field never closes a gap either,
 * so cells with water all round are also drawn back to rest; at the
 * surface that pull would act as a false surface tension. */
static void
relax_cell(flip3_t* f, int c, const relax_t* r) {
    float* const faces[3] = {f->u, f->v, f->w};
    float open_below[3];
    float open_above[3];
    float s = 0.0F;
    float div = 0.0F;
    for (int a = 0; a < 3; a++) {
        open_below[a] = open_toward(f, c - r->stride[a]);
        open_above[a] = open_toward(f, c + r->stride[a]);
        s += (open_below[a] + open_above[a]) * r->ih2[a];
        div += (faces[a][c + r->stride[a]] - faces[a][c]) * r->ih[a];
    }
    if (s == 0.0F) {
        return;
    }
    const float compression = f->density[c] - f->rest_density;
    if (compression > 0.0F || enclosed(f, c, r)) {
        div -= r->drift * compression;
    }
    const float p = -div / s * f->tuning.over_relaxation;
    for (int a = 0; a < 3; a++) {
        faces[a][c] -= open_below[a] * p * r->ih[a];
        faces[a][c + r->stride[a]] += open_above[a] * p * r->ih[a];
    }
}

/* Gauss-Seidel on the divergence over cells of any aspect, with the drift
 * push: a cell off rest density gets the tuned share of the difference as
 * flow per step. Water cells are never on the walls, so a flat walk in index
 * order visits them as the nested x, y, z walk would. */
static void
solve_pressure(flip3_t* f, float dt) {
    const vec3f_t ih = f->inv_h;
    const relax_t r = {{f->ny * f->nz, f->nz, 1},
                       {ih.x, ih.y, ih.z},
                       {ih.x * ih.x, ih.y * ih.y, ih.z * ih.z},
                       f->tuning.drift_gain / dt / f->rest_density};
    const int n = f->nx * f->ny * f->nz;
    for (int iter = 0; iter < f->tuning.pressure_iters; iter++) {
        for (int c = 0; c < n; c++) {
            if (f->kind[c] == KIND_FLUID) {
                relax_cell(f, c, &r);
            }
        }
    }
}

static void
to_particles(flip3_t* f) {
    component_t comps[3];
    components(f, comps);
    const float flip = f->tuning.flip_ratio;
    for (int i = 0; i < f->count; i++) {
        const place_t place = place_of(f, f->pos[i]);
        for (int a = 0; a < 3; a++) {
            const component_t* c = &comps[a];
            const stencil_t s = face_stencil(f, &place, a);
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

/* Charges the time since `since` to `stage` and returns now; 0 untimed. */
static int64_t
lap(const flip3_t* f, flip3_stage_t stage, int64_t since) {
    if (f->clock == NULL) {
        return 0;
    }
    const int64_t now = f->clock();
    f->stage_us[stage] += now - since;
    return now;
}

void
flip3_step(flip3_t* f, float dt_s, vec3f_t accel) {
    if (dt_s <= 0.0F || f->count == 0) {
        return;
    }
    int64_t at = f->clock != NULL ? f->clock() : 0;
    to_grid(f);
    at = lap(f, FLIP3_STAGE_P2G, at);
    accelerate(f, dt_s, accel);
    at = lap(f, FLIP3_STAGE_ACCEL, at);
    solve_pressure(f, dt_s);
    at = lap(f, FLIP3_STAGE_PRESSURE, at);
    to_particles(f);
    at = lap(f, FLIP3_STAGE_G2P, at);
    advect(f, dt_s);
    at = lap(f, FLIP3_STAGE_ADVECT, at);
    collide(f);
    (void)lap(f, FLIP3_STAGE_COLLIDE, at);
}

float
flip3_peak_speed_sq(const flip3_t* f) {
    float peak = 0.0F;
    for (int i = 0; i < f->count; i++) {
        peak = fmaxf(peak, vec3f_dot(f->vel[i], f->vel[i]));
    }
    return peak;
}

static void
field_dims(const flip3_t* f, int samples, int dims[3]) {
    dims[0] = ((f->nx - 2) * samples) + 2;
    dims[1] = ((f->ny - 2) * samples) + 2;
    dims[2] = ((f->nz - 2) * samples) + 2;
}

size_t
flip3_field_count(const flip3_t* f, int samples) {
    int d[3];
    field_dims(f, samples, d);
    return (size_t)d[0] * (size_t)d[1] * (size_t)d[2];
}

/* One [1 2 1] / 4 pass along one axis, inside the empty border, the walls
 * mirrored; in place, carrying the sample before. */
static void
smooth_axis(float* field, int stride, int count, int step_a, int count_a, int step_b, int count_b) {
    for (int a = 1; a < count_a - 1; a++) {
        for (int b = 1; b < count_b - 1; b++) {
            float* line = &field[(a * step_a) + (b * step_b)];
            float before = line[stride];
            for (int i = 1; i < count - 1; i++) {
                const float here = line[i * stride];
                const float after = i + 1 < count - 1 ? line[(i + 1) * stride] : here;
                line[i * stride] = 0.25F * (before + (2.0F * here) + after);
                before = here;
            }
        }
    }
}

iso_field_t
flip3_field(const flip3_t* f, int samples, float* out) {
    int d[3];
    field_dims(f, samples, d);
    const int sx = d[0];
    const int sy = d[1];
    const int sz = d[2];
    const vec3f_t step = vec3f_scale(f->h, 1.0F / (float)samples);
    const float inv_rest = 1.0F / f->rest_density;
    for (int x = 0; x < sx; x++) {
        for (int y = 0; y < sy; y++) {
            for (int z = 0; z < sz; z++) {
                float* at = &out[(((x * sy) + y) * sz) + z];
                if (x == 0 || y == 0 || z == 0 || x == sx - 1 || y == sy - 1 || z == sz - 1) {
                    *at = 0.0F;
                    continue;
                }
                /* Grid coordinates: the box starts one cell in. */
                const vec3f_t p = {f->h.x + (((float)x - 0.5F) * step.x), f->h.y + (((float)y - 0.5F) * step.y),
                                   f->h.z + (((float)z - 0.5F) * step.z)};
                const stencil_t s = centre_stencil(f, p);
                float density = 0.0F;
                for (int k = 0; k < 8; k++) {
                    density += s.weight[k] * f->density[s.at[k]];
                }
                *at = density * inv_rest;
            }
        }
    }
    smooth_axis(out, sy * sz, sx, sz, sy, 1, sz);
    smooth_axis(out, sz, sy, sy * sz, sx, 1, sz);
    smooth_axis(out, 1, sz, sy * sz, sx, sz, sy);
    /* Sample (1, 1, 1) sits half a sample in from the box's origin. */
    return (iso_field_t){out, sx, sy, sz, vec3f_scale(step, -0.5F), step};
}
