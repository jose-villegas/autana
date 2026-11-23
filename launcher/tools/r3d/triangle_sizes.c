#include "triangle_sizes.h"

#include <stdbool.h>

#include "render/r3d_span_internal.h"

typedef r3d_size_point_t point_t;

static bool
project(const r3d_lens_t* lens, const int16_t p[3], point_t* out) {
    const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
    if (l.z <= lens->project.near_z) {
        return false;
    }
    *out = (point_t){lens->project.cx + (l.x / l.z), lens->project.cy + (l.y / l.z)};
    return true;
}

static void
size_triangle(point_t v[3], bool double_sided, int width, int height, r3d_sizes_t* s) {
    const float area2 = r3d_size_area2(v[0], v[1], v[2]);
    if (area2 == 0.0F || (!double_sided && area2 > 0.0F)) {
        return; /* front faces wind negative on screen */
    }
    const recti_t b = r3d_size_centre_box(v);
    if (recti_right(b) <= 0 || b.x >= width || recti_bottom(b) <= 0 || b.y >= height) {
        return;
    }
    s->drawn++;
    s->box_empty += recti_is_empty(b);
    s->box_two_by_two += !recti_is_empty(b) && b.w <= 2 && b.h <= 2;
    s->bins[r3d_size_bin(v, b, width, height)]++;
}

void
r3d_sizes_count(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* visible, int count,
                r3d_sizes_t* out) {
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[visible[i]];
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            point_t v[3];
            bool in_front = true;
            for (int k = 0; k < 3; k++) {
                in_front = in_front && project(lens, mesh->positions[mesh->triangles[t][k]], &v[k]);
            }
            if (in_front) {
                size_triangle(v, c->double_sided, lens->width, lens->height, out);
            }
        }
    }
}

void
r3d_sizes_add(r3d_sizes_t* total, const r3d_sizes_t* s) {
    total->drawn += s->drawn;
    total->box_empty += s->box_empty;
    total->box_two_by_two += s->box_two_by_two;
    for (int b = 0; b < R3D_SIZES_BINS; b++) {
        total->bins[b] += s->bins[b];
    }
}

static int
box_side(int centres) {
    return centres < R3D_BOXES_SIDES ? centres - 1 : R3D_BOXES_SIDES - 1;
}

void
r3d_boxes_count(r3d_boxes_t* out, const r3d_span_target_t* target, const r3d_span_vertex_t* a,
                const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, bool solid) {
    const r3d_span_extent_t e = r3d_span_extent(a, b, c);
    const recti_t box = e.centres;
    if (recti_is_empty(r3d_span_window_box(target, box)) || r3d_span_area2(a, b, c) == 0) {
        return;
    }
    const int mode = e.flat ? R3D_BOXES_FLAT : (solid ? R3D_BOXES_FACE : R3D_BOXES_SMOOTH);
    out->boxes[mode][box_side(e.centres.h)][box_side(e.centres.w)]++;
}

/* Triangles of `mode`, or of every mode for R3D_BOXES_MODES, whose box is
 * at most n x n; all of them for n 0. */
static long
boxes_within(const r3d_boxes_t* boxes, int mode, int n) {
    const int last = n == 0 ? R3D_BOXES_SIDES : n;
    long sum = 0;
    for (int m = 0; m < R3D_BOXES_MODES; m++) {
        for (int rows = 0; rows < last && (m == mode || mode == R3D_BOXES_MODES); rows++) {
            for (int columns = 0; columns < last; columns++) {
                sum += boxes->boxes[m][rows][columns];
            }
        }
    }
    return sum;
}

void
r3d_boxes_print(FILE* f, const r3d_boxes_t* boxes) {
    static const char* const names[R3D_BOXES_MODES + 1] = {"flat", "face", "smooth", "all"};
    (void)fputs("| mode | triangles |", f);
    for (int n = 2; n < R3D_BOXES_SIDES; n++) {
        (void)fprintf(f, " box <= %dx%d |", n, n);
    }
    (void)fputs("\n|---|---:|", f);
    for (int n = 2; n < R3D_BOXES_SIDES; n++) {
        (void)fputs("---:|", f);
    }
    (void)fputs("\n", f);
    for (int mode = 0; mode <= R3D_BOXES_MODES; mode++) {
        const long all = boxes_within(boxes, mode, 0);
        (void)fprintf(f, "| %s | %ld |", names[mode], all);
        for (int n = 2; n < R3D_BOXES_SIDES; n++) {
            (void)fprintf(f, " %.1f%% |", all > 0 ? 100.0 * (double)boxes_within(boxes, mode, n) / (double)all : 0.0);
        }
        (void)fputs("\n", f);
    }
}
