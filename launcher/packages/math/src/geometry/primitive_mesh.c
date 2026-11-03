#include "math/geometry/primitive_mesh.h"

#include <math.h>

#include "math/scalar/math_const.h"

/* The most vertices a uint16 index reaches. */
#define INDEXABLE_VERTICES (UINT16_MAX + 1)

/* A count as an int, held at INT32_MAX past it so that a vast shape is
 * refused rather than overflowing. */
static int
held(int64_t count) {
    return count > INT32_MAX ? INT32_MAX : (int)count;
}

/* A parameter's count, held at INDEXABLE_VERTICES: a shape cut that many
 * times is already past what an index reaches, and its counts stay past it
 * without the products overflowing. */
static int64_t
cut(int count) {
    return count > INDEXABLE_VERTICES ? INDEXABLE_VERTICES : count;
}

/* Whether `counts` fit `out` and a uint16 index reaches every vertex. */
static bool
fits(const primitive_mesh_t* out, primitive_counts_t counts) {
    return counts.vertices <= out->vertex_capacity && counts.triangles <= out->triangle_capacity
           && counts.vertices <= INDEXABLE_VERTICES;
}

static void
begin(primitive_mesh_t* out) {
    out->vertex_count = 0;
    out->triangle_count = 0;
}

static int
add_vertex(primitive_mesh_t* out, vec3f_t at) {
    out->positions[out->vertex_count] = at;
    return out->vertex_count++;
}

static void
add_triangle(primitive_mesh_t* out, int a, int b, int c) {
    uint16_t* t = out->triangles[out->triangle_count++];
    t[0] = (uint16_t)a;
    t[1] = (uint16_t)b;
    t[2] = (uint16_t)c;
}

/* Grids: the plane and the cube's faces. */

static int64_t
grid_vertices(int subdivisions) {
    return (cut(subdivisions) + 1) * (cut(subdivisions) + 1);
}

static int64_t
grid_triangles(int subdivisions) {
    return 2 * cut(subdivisions) * cut(subdivisions);
}

/* `centre` + a unit square spanned by `u` and `v`, facing u x v. */
static void
add_grid(primitive_mesh_t* out, int subdivisions, vec3f_t centre, vec3f_t u, vec3f_t v) {
    const int first = out->vertex_count;
    const int side = subdivisions + 1;
    for (int j = 0; j < side; j++) {
        const float down = ((float)j / (float)subdivisions) - 0.5F;
        for (int i = 0; i < side; i++) {
            const float across = ((float)i / (float)subdivisions) - 0.5F;
            (void)add_vertex(out, vec3f_add(centre, vec3f_add(vec3f_scale(u, across), vec3f_scale(v, down))));
        }
    }
    for (int j = 0; j < subdivisions; j++) {
        for (int i = 0; i < subdivisions; i++) {
            const int corner = first + (j * side) + i;
            add_triangle(out, corner, corner + 1, corner + side + 1);
            add_triangle(out, corner, corner + side + 1, corner + side);
        }
    }
}

primitive_counts_t
primitive_plane_counts(const primitive_plane_t* shape) {
    return (primitive_counts_t){held(grid_vertices(shape->subdivisions)), held(grid_triangles(shape->subdivisions))};
}

bool
primitive_plane(const primitive_plane_t* shape, primitive_mesh_t* out) {
    if (shape->subdivisions < 1 || !fits(out, primitive_plane_counts(shape))) {
        return false;
    }
    begin(out);
    add_grid(out, shape->subdivisions, vec3f_zero(), vec3f_unit_x(), vec3f_unit_y());
    return true;
}

/* The cube's faces, one each way along each axis. */
#define CUBE_FACES 6

/* Axis 0, 1 or 2 as a unit vector. */
static vec3f_t
axis(int a) {
    return a == 0 ? vec3f_unit_x() : a == 1 ? vec3f_unit_y() : vec3f_unit_z();
}

primitive_counts_t
primitive_cube_counts(const primitive_cube_t* shape) {
    return (primitive_counts_t){held(CUBE_FACES * grid_vertices(shape->subdivisions)),
                                held(CUBE_FACES * grid_triangles(shape->subdivisions))};
}

bool
primitive_cube(const primitive_cube_t* shape, primitive_mesh_t* out) {
    if (shape->subdivisions < 1 || !fits(out, primitive_cube_counts(shape))) {
        return false;
    }
    begin(out);
    /* Face f looks along axis a = f / 2, + then -; its edges are the next
     * two axes, swapped for the - face, so that u x v is its normal. */
    for (int f = 0; f < CUBE_FACES; f++) {
        const int a = f / 2;
        const bool minus = (f % 2) != 0;
        const vec3f_t u = axis((a + 1) % 3);
        const vec3f_t v = axis((a + 2) % 3);
        add_grid(out, shape->subdivisions, vec3f_scale(axis(a), minus ? -0.5F : 0.5F), minus ? v : u, minus ? u : v);
    }
    return true;
}

/* Lathes: the sphere, the capsule and the cylinder's side. */

/* One ring of a lathe: its radius about the y axis and its height. */
typedef struct {
    float radius;
    float y;
} ring_t;

/* Ring `row` of a shape, from the top down. */
typedef ring_t (*ring_fn)(const void* shape, int row);

/* `bands` between bands + 1 rings. */
static int64_t
lathe_vertices(int64_t bands, int segments) {
    return (bands + 1) * (cut(segments) + 1);
}

/* Each band two triangles a segment, but one where its top or bottom ring is a point. */
static int64_t
lathe_triangles(int64_t bands, int segments, bool top_point, bool bottom_point) {
    return cut(segments) * ((2 * bands) - (top_point ? 1 : 0) - (bottom_point ? 1 : 0));
}

/* A ring's vertex at slice `s`, in the same place whichever part of a shape asks. */
static vec3f_t
ring_point(ring_t ring, int s, int segments) {
    const float round = MATH_TAU * (float)s / (float)segments;
    return (vec3f_t){ring.radius * cosf(round), ring.y, ring.radius * sinf(round)};
}

/* Rings `0` to `rows - 1` of `shape`, turned about y; a ring of radius 0
 * is a point, so the band beside it has one triangle a segment. Each band's
 * quad is a (r, s), b (r + 1, s), c (r + 1, s + 1), d (r, s + 1). */
static void
add_lathe(primitive_mesh_t* out, const void* shape, ring_fn ring, int rows, int segments) {
    const int first = out->vertex_count;
    const int row = segments + 1;
    for (int r = 0; r < rows; r++) {
        const ring_t at = ring(shape, r);
        for (int s = 0; s <= segments; s++) {
            (void)add_vertex(out, ring_point(at, s, segments));
        }
    }
    for (int r = 0; r + 1 < rows; r++) {
        const bool top_point = ring(shape, r).radius == 0.0F;
        const bool bottom_point = ring(shape, r + 1).radius == 0.0F;
        for (int s = 0; s < segments; s++) {
            const int a = first + (r * row) + s;
            if (!top_point) {
                add_triangle(out, a, a + 1, a + row + 1);
            }
            if (!bottom_point) {
                add_triangle(out, a, a + row + 1, a + row);
            }
        }
    }
}

static ring_t
sphere_ring(const void* shape, int row) {
    const int rings = ((const primitive_sphere_t*)shape)->rings;
    const float down = MATH_PI * (float)row / (float)rings;
    /* A pole's radius is 0 exactly, not sin(pi)'s rounding. */
    return (ring_t){row == 0 || row == rings ? 0.0F : sinf(down), cosf(down)};
}

static bool
sphere_valid(const primitive_sphere_t* shape) {
    return shape->segments >= PRIMITIVE_SEGMENTS_MIN && shape->rings >= PRIMITIVE_SPHERE_RINGS_MIN;
}

primitive_counts_t
primitive_sphere_counts(const primitive_sphere_t* shape) {
    return (primitive_counts_t){held(lathe_vertices(cut(shape->rings), shape->segments)),
                                held(lathe_triangles(cut(shape->rings), shape->segments, true, true))};
}

bool
primitive_sphere(const primitive_sphere_t* shape, primitive_mesh_t* out) {
    if (!sphere_valid(shape) || !fits(out, primitive_sphere_counts(shape))) {
        return false;
    }
    begin(out);
    add_lathe(out, shape, sphere_ring, shape->rings + 1, shape->segments);
    return true;
}

/* The top hemisphere's rings 0 to `rings`, then the bottom's, each about its own centre. */
static ring_t
capsule_ring(const void* shape, int row) {
    const primitive_capsule_t* c = shape;
    const bool top = row <= c->rings;
    const int down = top ? row : row - 1;
    const float angle = MATH_PI * 0.5F * (float)down / (float)c->rings;
    const float radius = down == 0 || down == 2 * c->rings ? 0.0F : c->radius * sinf(angle);
    return (ring_t){radius, ((top ? 0.5F : -0.5F) * c->height) + (c->radius * cosf(angle))};
}

static int
capsule_rows(const primitive_capsule_t* shape) {
    return (2 * shape->rings) + 2;
}

primitive_counts_t
primitive_capsule_counts(const primitive_capsule_t* shape) {
    /* Each hemisphere's bands and the cylinder's one between them. */
    const int64_t bands = (2 * cut(shape->rings)) + 1;
    return (primitive_counts_t){held(lathe_vertices(bands, shape->segments)),
                                held(lathe_triangles(bands, shape->segments, true, true))};
}

bool
primitive_capsule(const primitive_capsule_t* shape, primitive_mesh_t* out) {
    if (shape->segments < PRIMITIVE_SEGMENTS_MIN || shape->rings < 1 || !(shape->radius > 0.0F)
        || !(shape->height >= 0.0F) || !fits(out, primitive_capsule_counts(shape))) {
        return false;
    }
    begin(out);
    add_lathe(out, shape, capsule_ring, capsule_rows(shape), shape->segments);
    return true;
}

static ring_t
cylinder_ring(const void* shape, int row) {
    const primitive_cylinder_t* c = shape;
    const float t = (float)row / (float)c->stacks;
    return (ring_t){((1.0F - t) * c->top_radius) + (t * c->bottom_radius), (0.5F - t) * c->height};
}

/* The ends a cylinder closes: those of radius above 0. */
static int
cylinder_caps(const primitive_cylinder_t* shape) {
    return (shape->top_radius > 0.0F ? 1 : 0) + (shape->bottom_radius > 0.0F ? 1 : 0);
}

primitive_counts_t
primitive_cylinder_counts(const primitive_cylinder_t* shape) {
    return (primitive_counts_t){
        held(lathe_vertices(cut(shape->stacks), shape->segments) + (cylinder_caps(shape) * (cut(shape->segments) + 2))),
        held(lathe_triangles(cut(shape->stacks), shape->segments, shape->top_radius == 0.0F,
                             shape->bottom_radius == 0.0F)
             + (cylinder_caps(shape) * cut(shape->segments)))};
}

/* An end's fan: its centre and a ring of its own, facing up for the top. */
static void
add_cap(primitive_mesh_t* out, ring_t ring, int segments, bool top) {
    const int centre = add_vertex(out, (vec3f_t){0.0F, ring.y, 0.0F});
    for (int s = 0; s <= segments; s++) {
        (void)add_vertex(out, ring_point(ring, s, segments));
    }
    for (int s = 0; s < segments; s++) {
        const int a = centre + 1 + s;
        if (top) {
            add_triangle(out, centre, a + 1, a);
        } else {
            add_triangle(out, centre, a, a + 1);
        }
    }
}

bool
primitive_cylinder(const primitive_cylinder_t* shape, primitive_mesh_t* out) {
    if (shape->segments < PRIMITIVE_SEGMENTS_MIN || shape->stacks < 1 || !(shape->height > 0.0F)
        || !(shape->top_radius >= 0.0F) || !(shape->bottom_radius >= 0.0F) || cylinder_caps(shape) == 0
        || !fits(out, primitive_cylinder_counts(shape))) {
        return false;
    }
    begin(out);
    add_lathe(out, shape, cylinder_ring, shape->stacks + 1, shape->segments);
    if (shape->top_radius > 0.0F) {
        add_cap(out, cylinder_ring(shape, 0), shape->segments, true);
    }
    if (shape->bottom_radius > 0.0F) {
        add_cap(out, cylinder_ring(shape, shape->stacks), shape->segments, false);
    }
    return true;
}
