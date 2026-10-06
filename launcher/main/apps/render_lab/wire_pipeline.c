#include "wire_pipeline.h"

#include <assert.h>

void
wire_transform(const wire_mesh_t* mesh, const r3d_line_view_t* view, wire_frame_t* frame) {
    assert(mesh->vertex_count <= frame->cs_capacity);

    for (uint16_t i = 0; i < mesh->vertex_count; i++) {
        wire_cs_vertex_t* out = &frame->cs_vertices[i];
        out->cs = r3d_to_camera_space(mesh->vertices[i], view);
        if (out->cs.z > view->near_z) {
            int x, y;
            r3d_camera_to_screen(out->cs, view, &x, &y);
            out->pixel = (vec2i_t){x, y};
        }
    }
}

bool
wire_project_edges(const wire_mesh_t* mesh, const r3d_line_view_t* view, int screen_w, int screen_h,
                   wire_frame_t* frame) {
    frame->segment_count = 0;
    frame->bbox = GFX_BOX_EMPTY;

    for (uint16_t i = 0; i < mesh->edge_count; i++) {
        const wire_edge_t* edge = &mesh->edges[i];
        const wire_cs_vertex_t* a = &frame->cs_vertices[edge->a];
        const wire_cs_vertex_t* b = &frame->cs_vertices[edge->b];
        int ax = a->pixel.x;
        int ay = a->pixel.y;
        int bx = b->pixel.x;
        int by = b->pixel.y;

        const bool both_in_front = a->cs.z > view->near_z && b->cs.z > view->near_z;
        if (!both_in_front && !r3d_project_segment_cs(a->cs, b->cs, view, &ax, &ay, &bx, &by)) {
            continue;
        }

        if (!gfx_box_clip_segment((gfx_box_t){0, 0, screen_w, screen_h}, &ax, &ay, &bx, &by)) {
            continue;
        }
        if (frame->segment_count >= frame->segment_capacity) {
            return false;
        }

        gfx_box_extend(&frame->bbox,
                       (gfx_box_t){im_min(ax, bx), im_min(ay, by), im_max(ax, bx) + 1, im_max(ay, by) + 1});

        wire_segment_t* segment = &frame->segments[frame->segment_count++];
        segment->x0 = (int16_t)ax;
        segment->y0 = (int16_t)ay;
        segment->x1 = (int16_t)bx;
        segment->y1 = (int16_t)by;
    }
    return true;
}
