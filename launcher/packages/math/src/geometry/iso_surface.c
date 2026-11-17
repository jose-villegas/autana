#include "math/geometry/iso_surface.h"

#include <math.h>
#include <string.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

/* A cell's corner k sits at +x when bit 0 is set, +y bit 1, +z bit 2. */
#define CORNERS          8
#define EDGES            12
#define NO_VERTEX        (-1)
/* Indices are uint16_t. */
#define VERTEX_INDEX_MAX 65536

static const uint8_t EDGE_CORNERS[EDGES][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, /* along x */
    {0, 2}, {1, 3}, {4, 6}, {5, 7}, /* along y */
    {0, 4}, {1, 5}, {2, 6}, {3, 7}, /* along z */
};

typedef struct {
    const iso_field_t* field;
    float level;
    iso_mesh_t* out;
    int cy, cz; /* cells per axis in a slab */
    bool full;
} nets_t;

static float
value_at(const iso_field_t* f, int x, int y, int z) {
    return f->values[(((x * f->ny) + y) * f->nz) + z];
}

static int32_t*
slot(const nets_t* n, int x, int y, int z) {
    return &n->out
                ->scratch[((size_t)(x & 1) * (size_t)n->cy * (size_t)n->cz) + ((size_t)y * (size_t)n->cz) + (size_t)z];
}

static vec3f_t
corner_offset(int k) {
    return (vec3f_t){(float)(k & 1), (float)((k >> 1) & 1), (float)((k >> 2) & 1)};
}

/* How the corners rise along the axis whose corner bit is `bit`: the four
 * edges' differences summed, a gradient over the cell to a constant. */
static float
corner_rise(const float v[CORNERS], int bit) {
    float rise = 0.0F;
    for (int k = 0; k < CORNERS; k++) {
        if ((k & bit) == 0) {
            rise += v[k | bit] - v[k];
        }
    }
    return rise;
}

/* The cell's vertex at the mean of its edge crossings, and the outward
 * normal from the corners' gradient; NO_VERTEX when the surface misses the
 * cell or the vertices ran out. */
static int32_t
cell_vertex(nets_t* n, int x, int y, int z) {
    const iso_field_t* f = n->field;
    float v[CORNERS];
    int inside = 0;
    for (int k = 0; k < CORNERS; k++) {
        v[k] = value_at(f, x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1));
        inside += v[k] > n->level ? 1 : 0;
    }
    if (inside == 0 || inside == CORNERS) {
        return NO_VERTEX;
    }
    iso_mesh_t* out = n->out;
    if (out->vertex_count >= out->vertex_capacity || out->vertex_count >= VERTEX_INDEX_MAX) {
        n->full = true;
        return NO_VERTEX;
    }
    vec3f_t sum = {0};
    int crossings = 0;
    for (int e = 0; e < EDGES; e++) {
        const int a = EDGE_CORNERS[e][0];
        const int b = EDGE_CORNERS[e][1];
        if ((v[a] > n->level) == (v[b] > n->level)) {
            continue;
        }
        const float t = (n->level - v[a]) / (v[b] - v[a]);
        sum = vec3f_add(sum, vec3f_lerp(corner_offset(a), corner_offset(b), t));
        crossings++;
    }
    const vec3f_t local = vec3f_scale(sum, 1.0F / (float)crossings);
    const vec3f_t grad = {corner_rise(v, 1) / f->step.x, corner_rise(v, 2) / f->step.y, corner_rise(v, 4) / f->step.z};
    const int32_t index = out->vertex_count++;
    out->positions[index] =
        (vec3f_t){f->origin.x + (((float)x + local.x) * f->step.x), f->origin.y + (((float)y + local.y) * f->step.y),
                  f->origin.z + (((float)z + local.z) * f->step.z)};
    const float length = sqrtf(vec3f_dot(grad, grad));
    out->normals[index] = length > 0.0F ? vec3f_scale(grad, -1.0F / length) : (vec3f_t){0};
    return index;
}

/* A quad over four cells, counter-clockwise about +axis; flipped when the
 * inside is on the far side of the edge. */
static void
emit_quad(nets_t* n, int32_t c00, int32_t c10, int32_t c11, int32_t c01, bool flip) {
    if (c00 == NO_VERTEX || c10 == NO_VERTEX || c11 == NO_VERTEX || c01 == NO_VERTEX) {
        return;
    }
    iso_mesh_t* out = n->out;
    if (out->triangle_count + 2 > out->triangle_capacity) {
        n->full = true;
        return;
    }
    const uint16_t a = (uint16_t)c00;
    const uint16_t b = (uint16_t)(flip ? c01 : c10);
    const uint16_t c = (uint16_t)c11;
    const uint16_t d = (uint16_t)(flip ? c10 : c01);
    out->triangles[out->triangle_count][0] = a;
    out->triangles[out->triangle_count][1] = b;
    out->triangles[out->triangle_count][2] = c;
    out->triangle_count++;
    out->triangles[out->triangle_count][0] = a;
    out->triangles[out->triangle_count][1] = c;
    out->triangles[out->triangle_count][2] = d;
    out->triangle_count++;
}

/* Whether the edge from sample (x, y, z) one step along `axis` crosses the
 * level, and which way: true for inside at its start. */
static bool
edge_crosses(const nets_t* n, int x, int y, int z, int axis, bool* inside_first) {
    const iso_field_t* f = n->field;
    const bool first = value_at(f, x, y, z) > n->level;
    const bool second =
        value_at(f, x + (axis == 0 ? 1 : 0), y + (axis == 1 ? 1 : 0), z + (axis == 2 ? 1 : 0)) > n->level;
    *inside_first = first;
    return first != second;
}

/* For edges along each axis, the two other axes the quad steps back along:
 * its cells are p - b - c, p - c, p and p - b, counter-clockwise about +axis
 * in a right-handed frame. */
static const int8_t QUAD_STEPS[3][2][3] = {
    {{0, 1, 0}, {0, 0, 1}}, /* along x: back in y, then z */
    {{0, 0, 1}, {1, 0, 0}}, /* along y: back in z, then x */
    {{1, 0, 0}, {0, 1, 0}}, /* along z: back in x, then y */
};

static int32_t
back(const nets_t* n, int x, int y, int z, const int8_t* b, const int8_t* c) {
    return *slot(n, x - b[0] - c[0], y - b[1] - c[1], z - b[2] - c[2]);
}

/* Quads across the crossing edges along `axis` from sample plane x, whose
 * cells all lie in slabs x - 1 and x. An edge along the axis needs cells
 * back one step in each other axis, so those start at 1. */
static void
emit_axis(nets_t* n, int x, int axis) {
    const int8_t* b = QUAD_STEPS[axis][0];
    const int8_t* c = QUAD_STEPS[axis][1];
    const int8_t none[3] = {0, 0, 0};
    const int y0 = b[1] + c[1];
    const int z0 = b[2] + c[2];
    bool first;
    for (int y = y0; y < n->cy; y++) {
        for (int z = z0; z < n->cz; z++) {
            if (edge_crosses(n, x, y, z, axis, &first)) {
                emit_quad(n, back(n, x, y, z, b, c), back(n, x, y, z, none, c), back(n, x, y, z, none, none),
                          back(n, x, y, z, b, none), !first);
            }
        }
    }
}

/* Edges along x reach only slab x; along y and z they also reach x - 1. */
static void
emit_slab(nets_t* n, int x) {
    emit_axis(n, x, 0);
    if (x > 0) {
        emit_axis(n, x, 1);
        emit_axis(n, x, 2);
    }
}

/* A part after the first also makes the vertices of the slab before its
 * own, for the quads that reach back into it, but emits no quads there. */
bool
iso_surface_nets_slabs(const iso_field_t* field, float level, int x_from, int x_to, iso_mesh_t* out) {
    out->vertex_count = 0;
    out->triangle_count = 0;
    if (field->ny < 2 || field->nz < 2 || x_from < 0 || x_from >= x_to || x_to > field->nx - 1) {
        return false;
    }
    nets_t n = {.field = field, .level = level, .out = out, .cy = field->ny - 1, .cz = field->nz - 1};
    for (int x = x_from > 0 ? x_from - 1 : 0; x < x_to; x++) {
        for (int y = 0; y < n.cy; y++) {
            for (int z = 0; z < n.cz; z++) {
                *slot(&n, x, y, z) = cell_vertex(&n, x, y, z);
            }
        }
        if (x >= x_from) {
            emit_slab(&n, x);
        }
    }
    return !n.full;
}

bool
iso_surface_nets(const iso_field_t* field, float level, iso_mesh_t* out) {
    return iso_surface_nets_slabs(field, level, 0, field->nx - 1, out);
}

bool
iso_surface_append(iso_mesh_t* into, const iso_mesh_t* part) {
    if (into->vertex_count + part->vertex_count > into->vertex_capacity
        || into->vertex_count + part->vertex_count > VERTEX_INDEX_MAX
        || into->triangle_count + part->triangle_count > into->triangle_capacity) {
        return false;
    }
    const int base = into->vertex_count;
    memmove(&into->positions[base], part->positions, sizeof(vec3f_t) * (size_t)part->vertex_count);
    memmove(&into->normals[base], part->normals, sizeof(vec3f_t) * (size_t)part->vertex_count);
    for (int t = 0; t < part->triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            into->triangles[into->triangle_count + t][k] = (uint16_t)(part->triangles[t][k] + base);
        }
    }
    into->vertex_count += part->vertex_count;
    into->triangle_count += part->triangle_count;
    return true;
}
