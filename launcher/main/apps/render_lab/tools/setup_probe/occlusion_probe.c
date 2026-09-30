/* Measurement probe, never ships: the triangle-level occlusion ceiling and the
 * per-stage triangle counts of the lit-mesh draw, at the perf suite's poses.
 * r3d_lit_pipeline.c is compiled with r3d_span_triangle renamed to
 * probe_span_triangle, which wraps the real one. See build.sh. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_lite_mesh_generated.h"
#include "apps/render_lab/sponza_mesh_generated.h"
#include "render/r3d_lit_frame.h"

#define W SPONZA_RENDER_WIDTH
#define H SPONZA_RENDER_HEIGHT

static uint16_t save_d[W * H];
static uint16_t save_c[W * H];
static int32_t owner[W * H];
static uint8_t* shown; /* per span call of this pose: owns a final pixel */
static uint8_t* covering;
static int32_t serial;
static long calls, covers, cover_nowrite;

void
probe_span_triangle(const r3d_span_target_t* t, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                    const r3d_span_vertex_t* c) {
    int x0 = r3d_span_first_centre(r3d_span_min3(a->x, b->x, c->x));
    int x1 = r3d_span_first_centre(r3d_span_max3(a->x, b->x, c->x));
    int y0 = r3d_span_first_centre(r3d_span_min3(a->y, b->y, c->y));
    int y1 = r3d_span_first_centre(r3d_span_max3(a->y, b->y, c->y));
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > t->width ? t->width : x1;
    y0 = y0 < t->row0 ? t->row0 : y0;
    y1 = y1 > t->row1 ? t->row1 : y1;
    const int id = serial++;
    bool cov = false;
    bool wrote = false;
    calls++;
    if (x0 < x1 && y0 < y1) {
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                const int i = (y * W) + x;
                save_d[i] = t->depth[i];
                save_c[i] = t->color[i];
                t->depth[i] = 0;
            }
        }
        r3d_span_triangle(t, a, b, c);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                const int i = (y * W) + x;
                cov |= t->depth[i] != 0;
                t->depth[i] = save_d[i];
                t->color[i] = save_c[i];
            }
        }
        r3d_span_triangle(t, a, b, c);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                const int i = (y * W) + x;
                if (t->depth[i] != save_d[i]) {
                    wrote = true;
                    owner[i] = id;
                }
            }
        }
    }
    covering[id] = cov;
    covers += cov;
    cover_nowrite += cov && !wrote;
}

typedef struct {
    long submitted, in_front, rebuilt, behind, no_centre, facing, fast_to_span;
    long calls, covers, cover_nowrite, cover_hidden_final;
} tally_t;

static bool
misses(const r3d_lit_vertex_t* a, const r3d_lit_vertex_t* b, const r3d_lit_vertex_t* c) {
    const int x_first = r3d_span_first_centre(r3d_span_min3(a->sx, b->sx, c->sx));
    const int x_end = r3d_span_first_centre(r3d_span_max3(a->sx, b->sx, c->sx));
    const int y_first = r3d_span_first_centre(r3d_span_min3(a->sy, b->sy, c->sy));
    const int y_end = r3d_span_first_centre(r3d_span_max3(a->sy, b->sy, c->sy));
    return x_first >= x_end || y_first >= y_end || x_end <= 0 || x_first >= W || y_end <= 0 || y_first >= H;
}

static void
count_stages(const r3d_lit_mesh_t* mesh, const uint16_t* visible, int n, const r3d_lit_vertex_t* cs, tally_t* s) {
    for (int i = 0; i < n; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[visible[i]];
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const r3d_lit_vertex_t* v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
            s->submitted++;
            if (v[0]->iz > 0.0F && v[1]->iz > 0.0F && v[2]->iz > 0.0F) {
                s->in_front++;
                if (misses(v[0], v[1], v[2])) {
                    s->no_centre++;
                    continue;
                }
                const int32_t area2 =
                    ((v[1]->sx - v[0]->sx) * (v[2]->sy - v[0]->sy)) - ((v[2]->sx - v[0]->sx) * (v[1]->sy - v[0]->sy));
                if (!c->double_sided && area2 >= 0) {
                    s->facing++;
                    continue;
                }
                s->fast_to_span++;
            } else if (v[0]->iz != 0.0F || v[1]->iz != 0.0F || v[2]->iz != 0.0F) {
                s->rebuilt++;
            } else {
                s->behind++;
            }
        }
    }
}

static void
print_row(const char* label, const tally_t* s) {
    printf("%-10s submitted=%6ld in_front=%6ld rebuilt=%4ld behind=%4ld | no_centre=%6ld (%4.1f%%) facing=%6ld "
           "(%4.1f%%) fast_to_span=%6ld (%4.1f%%) | span_calls=%6ld covering=%6ld nowrite=%6ld (%4.1f%% of covering) "
           "hidden_in_final=%6ld (%4.1f%%)\n",
           label, s->submitted, s->in_front, s->rebuilt, s->behind, s->no_centre, 100.0 * s->no_centre / s->submitted,
           s->facing, 100.0 * s->facing / s->submitted, s->fast_to_span, 100.0 * s->fast_to_span / s->submitted,
           s->calls, s->covers, s->cover_nowrite, 100.0 * s->cover_nowrite / s->covers, s->cover_hidden_final,
           100.0 * s->cover_hidden_final / s->covers);
}

static void
run(const char* name, const r3d_lit_mesh_t* mesh) {
    uint16_t* color = malloc(sizeof(uint16_t) * W * H);
    uint16_t* depth = malloc(sizeof(uint16_t) * W * H);
    uint16_t* visible = malloc(sizeof(uint16_t) * mesh->cluster_count);
    r3d_lit_vertex_t* cs = malloc(sizeof(r3d_lit_vertex_t) * mesh->vertex_count);
    shown = malloc((size_t)mesh->triangle_count * 4);
    covering = malloc((size_t)mesh->triangle_count * 4);
    tally_t total = {0};
    printf("=== %s (%d tris) ===\n", name, mesh->triangle_count);
    for (uint32_t t_ms = 0; t_ms < sponza_flythrough_period_ms(); t_ms += SPONZA_POSE_EVERY_MS) {
        r3d_lit_view_t view;
        sponza_view_at(&view, t_ms, mesh->position_scale, 0);
        const int n = r3d_lit_cull_clusters(mesh, &view, visible);
        r3d_lit_transform(mesh, &view, visible, n, cs, NULL);
        memset(depth, 0, sizeof(uint16_t) * W * H);
        memset(color, 0, sizeof(uint16_t) * W * H);
        for (int i = 0; i < W * H; i++) {
            owner[i] = -1;
        }
        tally_t s = {0};
        count_stages(mesh, visible, n, cs, &s);
        serial = 0;
        const long c0 = calls, v0 = covers, n0 = cover_nowrite;
        const r3d_span_target_t target = {color, depth, W, 0, H};
        r3d_lit_draw(mesh, &view, visible, n, cs, NULL, &target);
        s.calls = calls - c0;
        s.covers = covers - v0;
        s.cover_nowrite = cover_nowrite - n0;
        memset(shown, 0, (size_t)serial);
        for (int i = 0; i < W * H; i++) {
            if (owner[i] >= 0) {
                shown[owner[i]] = 1;
            }
        }
        for (int id = 0; id < serial; id++) {
            s.cover_hidden_final += covering[id] && !shown[id];
        }
        char label[16];
        snprintf(label, sizeof label, "t=%us", (unsigned)(t_ms / 1000));
        print_row(label, &s);
        long* p = (long*)&total;
        const long* q = (const long*)&s;
        for (size_t k = 0; k < sizeof(tally_t) / sizeof(long); k++) {
            p[k] += q[k];
        }
    }
    print_row("all poses", &total);
    free(color);
    free(depth);
    free(visible);
    free(cs);
    free(shown);
    free(covering);
}

int
main(void) {
    run("sponza", &sponza_mesh);
    run("lite", &sponza_lite_mesh);
    return 0;
}
