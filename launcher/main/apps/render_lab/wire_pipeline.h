/*
 * wire_pipeline - transform-then-project stages a scene runs once per frame
 * to turn a wire_mesh_t into screen-space line segments for gfx_line(). No
 * allocation and no file-scope state: every buffer is the caller's, sized
 * and freed around entering and leaving a scene.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_project.h"
#include "wire_mesh.h"

typedef struct {
    int32_t x, y, z; /* camera space, one transform per vertex per frame */
} wire_cs_vertex_t;

typedef struct {
    int16_t x0, y0, x1, y1; /* screen space, narrowed after near and screen clip */
} wire_segment_t;

typedef struct {
    wire_cs_vertex_t* cs_vertices;
    uint16_t cs_capacity;
    wire_segment_t* segments;
    uint16_t segment_capacity;
    uint16_t segment_count;
    int bbox_x0, bbox_y0, bbox_x1, bbox_y1; /* half-open, clipped to the screen; valid if segment_count > 0 */
} wire_frame_t;

void wire_transform(const wire_mesh_t* mesh, const r3d_view_t* view, wire_frame_t* frame);

bool wire_project_edges(const wire_mesh_t* mesh, const r3d_view_t* view, int screen_w, int screen_h,
                        wire_frame_t* frame);

static inline bool
wire_segment_overlaps_rows(const wire_segment_t* segment, int row0, int row1) {
    const int y_min = segment->y0 < segment->y1 ? segment->y0 : segment->y1;
    const int y_max = segment->y0 > segment->y1 ? segment->y0 : segment->y1;
    return y_max >= row0 && y_min < row1;
}
