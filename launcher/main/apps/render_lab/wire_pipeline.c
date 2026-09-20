/*
 * wire_pipeline - see wire_pipeline.h. Screen-space clipping runs in 64-bit
 * math before narrowing to wire_segment_t's int16 fields: a near-plane
 * crossing can project to a coordinate far past int16 range even though the
 * visible part of the line never does, the same overflow r3d_project.h's
 * own comment on r3d_camera_to_screen() names.
 */
#include "wire_pipeline.h"

#include <assert.h>

#define OUT_LEFT   1u
#define OUT_RIGHT  2u
#define OUT_TOP    4u
#define OUT_BOTTOM 8u

static unsigned
screen_outcode(int64_t x, int64_t y, int screen_w, int screen_h) {
    unsigned code = 0;

    if (x < 0) {
        code |= OUT_LEFT;
    } else if (x >= screen_w) {
        code |= OUT_RIGHT;
    }
    if (y < 0) {
        code |= OUT_TOP;
    } else if (y >= screen_h) {
        code |= OUT_BOTTOM;
    }
    return code;
}

/* Cohen-Sutherland against the screen rect. False means both ends share an
 * outside edge, so the segment is entirely off one side. */
static bool
clip_to_screen(int64_t* x0, int64_t* y0, int64_t* x1, int64_t* y1, int screen_w, int screen_h) {
    unsigned c0 = screen_outcode(*x0, *y0, screen_w, screen_h);
    unsigned c1 = screen_outcode(*x1, *y1, screen_w, screen_h);

    for (int pass = 0; pass < 8; pass++) {
        if ((c0 | c1) == 0) {
            return true;
        }
        if ((c0 & c1) != 0) {
            return false;
        }

        const unsigned out = c0 ? c0 : c1;
        int64_t x, y;

        if (out & OUT_BOTTOM) {
            y = screen_h - 1;
            x = *x0 + ((*x1 - *x0) * (y - *y0)) / (*y1 - *y0);
        } else if (out & OUT_TOP) {
            y = 0;
            x = *x0 + ((*x1 - *x0) * (y - *y0)) / (*y1 - *y0);
        } else if (out & OUT_RIGHT) {
            x = screen_w - 1;
            y = *y0 + ((*y1 - *y0) * (x - *x0)) / (*x1 - *x0);
        } else {
            x = 0;
            y = *y0 + ((*y1 - *y0) * (x - *x0)) / (*x1 - *x0);
        }

        if (out == c0) {
            *x0 = x;
            *y0 = y;
            c0 = screen_outcode(x, y, screen_w, screen_h);
        } else {
            *x1 = x;
            *y1 = y;
            c1 = screen_outcode(x, y, screen_w, screen_h);
        }
    }
    return false;
}

static void
expand_bbox(wire_frame_t* frame, int x0, int y0, int x1, int y1) {
    const int min_x = x0 < x1 ? x0 : x1;
    const int max_x = (x0 > x1 ? x0 : x1) + 1;
    const int min_y = y0 < y1 ? y0 : y1;
    const int max_y = (y0 > y1 ? y0 : y1) + 1;

    if (frame->segment_count == 0) {
        frame->bbox_x0 = min_x;
        frame->bbox_y0 = min_y;
        frame->bbox_x1 = max_x;
        frame->bbox_y1 = max_y;
        return;
    }
    if (min_x < frame->bbox_x0) {
        frame->bbox_x0 = min_x;
    }
    if (min_y < frame->bbox_y0) {
        frame->bbox_y0 = min_y;
    }
    if (max_x > frame->bbox_x1) {
        frame->bbox_x1 = max_x;
    }
    if (max_y > frame->bbox_y1) {
        frame->bbox_y1 = max_y;
    }
}

void
wire_transform(const wire_mesh_t* mesh, const r3d_view_t* view, wire_frame_t* frame) {
    assert(mesh->vertex_count <= frame->cs_capacity);

    for (uint16_t i = 0; i < mesh->vertex_count; i++) {
        const wire_vertex_t* v = &mesh->vertices[i];
        const S3L_Vec4 model_point = {v->x, v->y, v->z, S3L_F};
        const S3L_Vec4 cs = r3d_to_camera_space(model_point, view);

        frame->cs_vertices[i].x = cs.x;
        frame->cs_vertices[i].y = cs.y;
        frame->cs_vertices[i].z = cs.z;
    }
}

bool
wire_project_edges(const wire_mesh_t* mesh, const r3d_view_t* view, int screen_w, int screen_h, wire_frame_t* frame) {
    frame->segment_count = 0;

    for (uint16_t i = 0; i < mesh->edge_count; i++) {
        const wire_edge_t* edge = &mesh->edges[i];
        const wire_cs_vertex_t* a = &frame->cs_vertices[edge->a];
        const wire_cs_vertex_t* b = &frame->cs_vertices[edge->b];
        const S3L_Vec4 p0 = {a->x, a->y, a->z, S3L_F};
        const S3L_Vec4 p1 = {b->x, b->y, b->z, S3L_F};
        int ax, ay, bx, by;

        if (!r3d_project_segment_cs(p0, p1, view, &ax, &ay, &bx, &by)) {
            continue;
        }

        int64_t x0 = ax, y0 = ay, x1 = bx, y1 = by;
        if (!clip_to_screen(&x0, &y0, &x1, &y1, screen_w, screen_h)) {
            continue;
        }
        if (frame->segment_count >= frame->segment_capacity) {
            return false;
        }

        expand_bbox(frame, (int)x0, (int)y0, (int)x1, (int)y1);

        wire_segment_t* segment = &frame->segments[frame->segment_count++];
        segment->x0 = (int16_t)x0;
        segment->y0 = (int16_t)y0;
        segment->x1 = (int16_t)x1;
        segment->y1 = (int16_t)y1;
    }
    return true;
}
