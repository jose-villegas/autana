#include "math/geometry/primitive_mesh.h"

#include <math.h>

#include "math/scalar/math_const.h"

/* Whether `vertices` and `triangles` fit `out` and a uint16 index reaches every vertex. */
static bool
fits(const primitive_mesh_t* out, int vertices, int triangles) {
    return vertices <= out->vertex_capacity && triangles <= out->triangle_capacity && vertices <= UINT16_MAX + 1;
}

static void
add_triangle(primitive_mesh_t* out, int a, int b, int c) {
    uint16_t* t = out->triangles[out->triangle_count++];
    t[0] = (uint16_t)a;
    t[1] = (uint16_t)b;
    t[2] = (uint16_t)c;
}

bool
primitive_grid(int cells, primitive_mesh_t* out) {
    if (cells < 1 || !fits(out, primitive_grid_vertex_count(cells), primitive_grid_triangle_count(cells))) {
        return false;
    }
    const int side = cells + 1;
    out->vertex_count = 0;
    out->triangle_count = 0;
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            out->positions[out->vertex_count++] =
                (vec3f_t){((float)i / (float)cells) - 0.5F, ((float)j / (float)cells) - 0.5F, 0.0F};
        }
    }
    for (int j = 0; j < cells; j++) {
        for (int i = 0; i < cells; i++) {
            const int corner = (j * side) + i;
            add_triangle(out, corner, corner + 1, corner + side + 1);
            add_triangle(out, corner, corner + side + 1, corner + side);
        }
    }
    return true;
}

bool
primitive_sphere(int rings, primitive_mesh_t* out) {
    if (rings < PRIMITIVE_SPHERE_RINGS_MIN
        || !fits(out, primitive_sphere_vertex_count(rings), primitive_sphere_triangle_count(rings))) {
        return false;
    }
    const int segments = primitive_sphere_segments(rings);
    const int row = segments + 1;
    out->vertex_count = 0;
    out->triangle_count = 0;
    for (int r = 0; r <= rings; r++) {
        const float down = MATH_PI * (float)r / (float)rings;
        for (int s = 0; s <= segments; s++) {
            const float round = MATH_TAU * (float)s / (float)segments;
            out->positions[out->vertex_count++] =
                (vec3f_t){sinf(down) * cosf(round), cosf(down), sinf(down) * sinf(round)};
        }
    }
    /* Each band's quad a (r, s), b (r + 1, s), c (r + 1, s + 1), d (r, s + 1);
     * a cap's quad has two corners on its pole, so only one triangle has area. */
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segments; s++) {
            const int a = (r * row) + s;
            if (r > 0) {
                add_triangle(out, a, a + 1, a + row + 1);
            }
            if (r < rings - 1) {
                add_triangle(out, a, a + row + 1, a + row);
            }
        }
    }
    return true;
}
