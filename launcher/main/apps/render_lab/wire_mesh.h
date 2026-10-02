/*
 * wire_mesh: an edge list a wireframe pipeline projects, not a triangle
 * mesh. An edge shared by two faces appears once, so a solid's cost is its
 * silhouette and creases, never its face count. Model space only, in
 * model units.
 */
#pragma once

#include <stdint.h>

#include "util/math/vec3f.h"

typedef struct {
    uint16_t a, b;
} wire_edge_t;

typedef struct {
    const vec3f_t* vertices;
    const wire_edge_t* edges;
    uint16_t vertex_count;
    uint16_t edge_count;
} wire_mesh_t;
