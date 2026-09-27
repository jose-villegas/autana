/*
 * sponza_visibility - how much of what passes the view test is actually
 * seen, at evenly spaced points of the flythrough. Host only.
 *
 * Each point is drawn whole, then every cluster the walk kept is drawn again
 * on its own into a scratch depth buffer: a cluster owns a pixel where its
 * own depth reaches the final one. The clusters owning none are the ceiling
 * on what occlusion culling can remove. Also counts the fill each cluster
 * asks for, and the screen areas of the triangles that reach the rasterizer.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/r3d_lit_pipeline.h"
#include "sponza_flythrough.h"
#include "sponza_mesh_generated.h"

#define W            368
#define H            448
#define HALF_FOV     0.62f
#define NEAR_Z       6.0f
#define AREA_BUCKETS 6

static gfx_color_t color[W * H];
static uint16_t depth[W * H];
static uint16_t scratch[W * H];
static gfx_color_t scratch_color[W * H];
static r3d_lit_vertex_t cs[SPONZA_VERTEX_COUNT];
static uint16_t visible[SPONZA_CLUSTER_COUNT];

static const char* const area_names[AREA_BUCKETS] = {"<1", "1-4", "4-16", "16-64", "64-256", ">256"};

static int
area_bucket(float area) {
    int b = 0;
    for (float edge = 1.0f; b < AREA_BUCKETS - 1 && area >= edge; edge *= 4.0f) {
        b++;
    }
    return b;
}

typedef struct {
    int in_view, facing, clipped;
    long area_count[AREA_BUCKETS];
} triangle_stats_t;

static void
count_triangles(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* clusters, int n,
                triangle_stats_t* st) {
    for (int i = 0; i < n; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const r3d_lit_vertex_t *a = &cs[tri[0]], *b = &cs[tri[1]], *d = &cs[tri[2]];
            st->in_view++;
            if (a->z <= view->near_z || b->z <= view->near_z || d->z <= view->near_z) {
                st->clipped++;
                continue;
            }
            const float area2 = (b->sx - a->sx) * (d->sy - a->sy) - (d->sx - a->sx) * (b->sy - a->sy);
            if (!c->double_sided && area2 >= 0.0f) {
                continue;
            }
            st->facing++;
            st->area_count[area_bucket(0.5f * fabsf(area2))]++;
        }
    }
}

int
main(int argc, char** argv) {
    const int step_ms = argc > 1 ? atoi(argv[1]) : 5000;
    const r3d_lit_mesh_t* mesh = &sponza_mesh;
    const uint32_t period = r3d_path_period_ms(&sponza_flythrough);
    const r3d_span_target_t full = {color, depth, W, 0, H};
    const r3d_span_target_t alone = {scratch_color, scratch, W, 0, H};

    printf("%6s %8s %8s %8s %8s %9s %9s %9s\n", "t(s)", "clusters", "seen", "tris", "seen", "fill px", "seen px",
           "overdraw");
    long sum_kept = 0, sum_seen = 0, sum_tris = 0, sum_seen_tris = 0, sum_fill = 0, sum_seen_fill = 0;
    triangle_stats_t all = {0};
    for (uint32_t t = 0; t < period; t += (uint32_t)step_ms) {
        r3d_lit_vec3_t eye, forward;
        r3d_path_sample(&sponza_flythrough, t, &eye, &forward);
        r3d_lit_view_t view;
        r3d_lit_view_look(&view, eye, forward, HALF_FOV, NEAR_Z, mesh->position_scale, W, H, 0);

        const int kept = r3d_lit_cull_clusters(mesh, &view, visible);
        r3d_lit_transform(mesh, &view, visible, kept, cs, NULL);
        memset(depth, 0, sizeof depth);
        r3d_lit_draw(mesh, &view, visible, kept, cs, NULL, &full);
        count_triangles(mesh, &view, visible, kept, &all);

        int seen = 0, tris = 0, seen_tris = 0;
        long fill = 0, seen_fill = 0;
        for (int i = 0; i < kept; i++) {
            memset(scratch, 0, sizeof scratch);
            r3d_lit_draw(mesh, &view, &visible[i], 1, cs, NULL, &alone);
            long px = 0;
            bool owns = false;
            for (int p = 0; p < W * H; p++) {
                if (scratch[p] != 0) {
                    px++;
                    owns |= scratch[p] >= depth[p];
                }
            }
            const int n = mesh->clusters[visible[i]].triangle_count;
            tris += n;
            fill += px;
            if (owns) {
                seen++;
                seen_tris += n;
                seen_fill += px;
            }
        }
        long covered = 0;
        for (int p = 0; p < W * H; p++) {
            covered += depth[p] != 0;
        }
        printf("%6.1f %8d %8d %8d %8d %9ld %9ld %9.2f\n", (double)t / 1000.0, kept, seen, tris, seen_tris, fill,
               seen_fill, covered ? (double)fill / (double)covered : 0.0);
        sum_kept += kept;
        sum_seen += seen;
        sum_tris += tris;
        sum_seen_tris += seen_tris;
        sum_fill += fill;
        sum_seen_fill += seen_fill;
    }
    printf("mean: %.0f%% of kept clusters seen, %.0f%% of their triangles, %.0f%% of their fill\n",
           100.0 * (double)sum_seen / (double)sum_kept, 100.0 * (double)sum_seen_tris / (double)sum_tris,
           100.0 * (double)sum_seen_fill / (double)sum_fill);
    printf("triangles in kept clusters: %d, near-clipped %d, facing the camera %d\n", all.in_view, all.clipped,
           all.facing);
    printf("screen area of facing triangles (px):");
    for (int b = 0; b < AREA_BUCKETS; b++) {
        printf(" %s %.0f%%", area_names[b], 100.0 * (double)all.area_count[b] / (double)all.facing);
    }
    printf("\n");
    return 0;
}
