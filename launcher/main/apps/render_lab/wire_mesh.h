/*
 * wire_mesh - an edge list a wireframe pipeline projects, not a triangle
 * mesh. An edge shared by two faces appears once, so a solid's cost is its
 * silhouette and creases, never its face count. Model space only, in
 * S3L_F units; no small3dlib type appears here so a caller can use this
 * without pulling the rasterizer config in - see render/r3d_project.h.
 */
#pragma once

#include <stdint.h>

typedef struct {
    int16_t x, y, z;
} wire_vertex_t;

typedef struct {
    uint16_t a, b;
} wire_edge_t;

typedef struct {
    const wire_vertex_t* vertices;
    const wire_edge_t* edges;
    uint16_t vertex_count;
    uint16_t edge_count;
} wire_mesh_t;
