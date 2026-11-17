/* contact_mask: top-down posed triangles filled with the span rasterizer's edges. */
#include "render/contact/contact_mask.h"

#include <math.h>
#include <string.h>
#include "render/r3d_span_internal.h"

/* Pose refits build masks serially after the skin workers join, keeping scratch off the main task stack. */
#define CONTACT_MASK_BLUR_SIDE 3
#define CONTACT_MASK_BLUR_AREA (CONTACT_MASK_BLUR_SIDE * CONTACT_MASK_BLUR_SIDE)

static r3d_span_vertex_t
mask_vertex(const contact_mask_t* mask, const int16_t position[3], int scale, const mat4f_t* world) {
    const vec3f_t local = {(float)position[0] / scale, (float)position[1] / scale, (float)position[2] / scale};
    const vec3f_t p = vec3f_sub(mat4f_apply(world, local), mask->origin);
    return (r3d_span_vertex_t){
        .x = r3d_span_snap(p.x * mask->side / mask->u.x), .y = r3d_span_snap(p.z * mask->side / mask->v.z), .z = p.y};
}

static void
mask_row(contact_mask_t* mask, const r3d_span_edge_t edges[3], int y, recti_t box) {
    const int cy = y * R3D_SUBPIXEL + R3D_SPAN_HALF_PIXEL;
    for (int x = mathi_max(0, box.x), end = mathi_min(mask->side, recti_right(box)); x < end; x++) {
        const int cx = x * R3D_SUBPIXEL + R3D_SPAN_HALF_PIXEL;
        if ((r3d_span_side(&edges[0], cx, cy) | r3d_span_side(&edges[1], cx, cy) | r3d_span_side(&edges[2], cx, cy))
            < 0) {
            continue;
        }
        mask->coverage[y * mask->side + x] = UINT8_MAX;
    }
}

static void
mask_triangle(contact_mask_t* mask, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
              const r3d_span_vertex_t* c) {
    if (r3d_span_area2(a, b, c) < 0) {
        const r3d_span_vertex_t* swap = b;
        b = c;
        c = swap;
    }
    if (r3d_span_area2(a, b, c) == 0) {
        return;
    }
    static r3d_span_edge_t edges[3];
    edges[0] = r3d_span_edge(a, b);
    edges[1] = r3d_span_edge(b, c);
    edges[2] = r3d_span_edge(c, a);
    const recti_t box = r3d_span_extent_limits(a, b, c, 0, 0).centres;
    const int end = mathi_min(mask->side, recti_bottom(box));
    for (int y = mathi_max(0, box.y); y < end; y++) {
        mask_row(mask, edges, y, box);
    }
}

static void
mask_cluster(contact_mask_t* mask, const r3d_lit_mesh_t* mesh, const r3d_lit_cluster_t* cluster, const mat4f_t* world) {
    for (int t = cluster->triangle_first; t < cluster->triangle_first + cluster->triangle_count; t++) {
        static r3d_span_vertex_t corners[3];
        for (int i = 0; i < 3; i++) {
            const int v = mesh->triangles[t][i];
            corners[i] = mask_vertex(mask, mesh->positions[v], mesh->position_scale, world);
        }
        mask_triangle(mask, &corners[0], &corners[1], &corners[2]);
    }
}

static void
mask_blur(contact_mask_t* mask) {
    static uint8_t rows[CONTACT_MASK_BLUR_SIDE][CONTACT_MASK_SIDE];
    memset(rows, 0, sizeof rows);
    memcpy(rows[1], mask->coverage, (size_t)mask->side);
    for (int y = 0; y < mask->side; y++) {
        uint8_t* next = rows[(y + 2) % CONTACT_MASK_BLUR_SIDE];
        memset(next, 0, (size_t)mask->side);
        if (y + 1 < mask->side) {
            memcpy(next, &mask->coverage[(y + 1) * mask->side], (size_t)mask->side);
        }
        for (int x = 0; x < mask->side; x++) {
            unsigned sum = 0;
            for (int r = 0; r < CONTACT_MASK_BLUR_SIDE; r++) {
                for (int xx = mathi_max(0, x - 1); xx <= mathi_min(mask->side - 1, x + 1); xx++) {
                    sum += rows[r][xx];
                }
            }
            mask->coverage[y * mask->side + x] = (uint8_t)(sum / CONTACT_MASK_BLUR_AREA);
        }
    }
}

void
contact_mask_build(contact_mask_t* mask, const r3d_lit_mesh_t* mesh, const mat4f_t* world, box3f_t mover,
                   float ground_y) {
    mask->side = CONTACT_MASK_SIDE;
    mask->origin = (vec3f_t){mover.lo.x, ground_y, mover.lo.z};
    mask->u = (vec3f_t){mover.hi.x - mover.lo.x, 0, 0};
    mask->v = (vec3f_t){0, 0, mover.hi.z - mover.lo.z};
    memset(mask->coverage, 0, CONTACT_MASK_BYTES);
    if (!(mask->u.x > 0 && mask->v.z > 0)) {
        return;
    }
    for (int i = 0; i < mesh->cluster_count; i++) {
        mask_cluster(mask, mesh, &mesh->clusters[i], world);
    }
    mask_blur(mask);
}
