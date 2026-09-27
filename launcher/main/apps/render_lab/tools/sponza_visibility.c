/*
 * sponza_visibility - how much of what passes the view test is actually
 * seen, at evenly spaced points of the flythrough. Host only.
 *
 *     sponza_visibility [step_ms]                 occlusion report
 *     sponza_visibility step_ms px [px ...]       level of detail report
 *
 * The occlusion report draws each point whole, then every cluster the walk
 * kept again on its own into a scratch depth buffer: a cluster owns a pixel
 * where its own depth reaches the final one. The clusters owning none are
 * the ceiling on what occlusion culling can remove. Also counts the fill each
 * cluster asks for, and the screen areas of the triangles that reach the
 * rasterizer.
 *
 * The level of detail report walks the path once per proxy error budget in
 * pixels (0 is full detail) and compares the triangles submitted and their
 * screen areas.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lit_pipeline.h"
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
static lit_cs_vertex_t cs[SPONZA_VERTEX_COUNT];
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
    long in_view, facing, clipped;
    long area_count[AREA_BUCKETS];
} triangle_stats_t;

static void
count_triangles(const lit_mesh_t* mesh, const lit_view_t* view, const uint16_t* clusters, int n, triangle_stats_t* st) {
    for (int i = 0; i < n; i++) {
        const lit_cluster_t* c = &mesh->clusters[clusters[i]];
        const lit_cs_vertex_t* own = cs + c->vertex_first;
        for (uint32_t t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const lit_cs_vertex_t *a = &own[tri[0]], *b = &own[tri[1]], *d = &own[tri[2]];
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

static lit_view_t
path_view(const lit_mesh_t* mesh, uint32_t t) {
    lit_vec3_t eye, forward;
    camera_path_sample(&sponza_flythrough, t, &eye, &forward);
    lit_view_t view;
    lit_view_look(&view, eye, forward, HALF_FOV, NEAR_Z, mesh->position_scale, W, H, 0);
    return view;
}

static void
print_areas(const triangle_stats_t* st) {
    for (int b = 0; b < AREA_BUCKETS; b++) {
        printf(" %s %.0f%%", area_names[b], 100.0 * (double)st->area_count[b] / (double)st->facing);
    }
}

static void
lod_report(const lit_mesh_t* mesh, int step_ms, int budgets, char** budget_args) {
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    printf("%6s", "t(s)");
    for (int b = 0; b < budgets; b++) {
        printf(" %9s@%-4s", "tris", budget_args[b]);
    }
    printf("\n");
    triangle_stats_t* totals = calloc((size_t)budgets, sizeof *totals);
    for (uint32_t t = 0; t < period; t += (uint32_t)step_ms) {
        const lit_view_t view = path_view(mesh, t);
        printf("%6.1f", (double)t / 1000.0);
        for (int b = 0; b < budgets; b++) {
            const int kept = lit_cull_clusters_lod(mesh, &view, (float)atof(budget_args[b]), visible);
            lit_transform(mesh, &view, visible, kept, cs, NULL);
            triangle_stats_t here = {0};
            count_triangles(mesh, &view, visible, kept, &here);
            printf(" %14ld", here.in_view);
            totals[b].in_view += here.in_view;
            totals[b].facing += here.facing;
            totals[b].clipped += here.clipped;
            for (int k = 0; k < AREA_BUCKETS; k++) {
                totals[b].area_count[k] += here.area_count[k];
            }
        }
        printf("\n");
    }
    for (int b = 0; b < budgets; b++) {
        printf("budget %s px: %ld triangles submitted, %ld facing; facing areas (px):", budget_args[b],
               totals[b].in_view, totals[b].facing);
        print_areas(&totals[b]);
        printf("\n");
    }
    free(totals);
}

int
main(int argc, char** argv) {
    const int step_ms = argc > 1 ? atoi(argv[1]) : 5000;
    const lit_mesh_t* mesh = &sponza_mesh;
    if (argc > 2) {
        lod_report(mesh, step_ms, argc - 2, argv + 2);
        return 0;
    }
    const uint32_t period = camera_path_period_ms(&sponza_flythrough);
    const span_target_t full = {color, depth, W, 0, H};
    const span_target_t alone = {scratch_color, scratch, W, 0, H};

    printf("%6s %8s %8s %8s %8s %9s %9s %9s\n", "t(s)", "clusters", "seen", "tris", "seen", "fill px", "seen px",
           "overdraw");
    long sum_kept = 0, sum_seen = 0, sum_tris = 0, sum_seen_tris = 0, sum_fill = 0, sum_seen_fill = 0;
    triangle_stats_t all = {0};
    for (uint32_t t = 0; t < period; t += (uint32_t)step_ms) {
        const lit_view_t view = path_view(mesh, t);
        const int kept = lit_cull_clusters(mesh, &view, visible);
        lit_transform(mesh, &view, visible, kept, cs, NULL);
        memset(depth, 0, sizeof depth);
        lit_draw(mesh, &view, visible, kept, cs, NULL, &full);
        count_triangles(mesh, &view, visible, kept, &all);

        int seen = 0, tris = 0, seen_tris = 0;
        long fill = 0, seen_fill = 0;
        for (int i = 0; i < kept; i++) {
            memset(scratch, 0, sizeof scratch);
            lit_draw(mesh, &view, &visible[i], 1, cs, NULL, &alone);
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
    printf("triangles in kept clusters: %ld, near-clipped %ld, facing the camera %ld\n", all.in_view, all.clipped,
           all.facing);
    printf("screen area of facing triangles (px):");
    print_areas(&all);
    printf("\n");
    return 0;
}
