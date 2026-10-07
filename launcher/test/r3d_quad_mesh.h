/*
 * r3d_quad_mesh: four corners and two triangles as a one-cluster lit mesh,
 * for suites that build their meshes inside the test.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/r3d_lit_mesh.h"

/* `positions`' corners, joined by `triangles` or, when NULL, 0-1-2 and
 * 0-2-3; lit white per vertex unless `face_colors` gives each face its own.
 * `cluster` and `node` hold its bounds. */
static inline r3d_lit_mesh_t
r3d_quad_mesh(const int16_t (*positions)[3], const uint16_t (*triangles)[3], const uint16_t* face_colors,
              const r3d_lit_cluster_t* cluster, const r3d_lit_node_t* node) {
    static const uint8_t white[4][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    static const uint16_t front[2][3] = {{0, 1, 2}, {0, 2, 3}};
    return (r3d_lit_mesh_t){.positions = positions,
                            .colors = face_colors == NULL ? white : NULL,
                            .triangles = triangles == NULL ? front : triangles,
                            .clusters = cluster,
                            .nodes = node,
                            .vertex_count = 4,
                            .triangle_count = 2,
                            .cluster_count = 1,
                            .node_count = 1,
                            .position_scale = 1,
                            .face_colors = face_colors};
}

/* A double-sided white quad with its bounds worked out from its corners; it
 * points into itself, so it stays where it was built. */
typedef struct {
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
    r3d_lit_mesh_t mesh;
} r3d_quad_t;

static inline void
r3d_quad_init(r3d_quad_t* q, const int16_t (*positions)[3]) {
    int16_t lo[3] = {INT16_MAX, INT16_MAX, INT16_MAX};
    int16_t hi[3] = {INT16_MIN, INT16_MIN, INT16_MIN};
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 3; k++) {
            lo[k] = positions[i][k] < lo[k] ? positions[i][k] : lo[k];
            hi[k] = positions[i][k] > hi[k] ? positions[i][k] : hi[k];
        }
    }
    q->cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}, true};
    q->node = (r3d_lit_node_t){{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}, 0, 1, true};
    q->mesh = r3d_quad_mesh(positions, NULL, NULL, &q->cluster, &q->node);
}
