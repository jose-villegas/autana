/*
 * Sizes Sponza's drawn triangles at the perf suite's poses by the pixel
 * centres they cover (top-left rule, counted from the view, so any pipeline
 * version means the same), and keeps or diffs each pose's rendered frame.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/render_lab/sponza_flythrough.h"
#include "apps/render_lab/sponza_mesh_generated.h"
#include "render/r3d_lit_pipeline.h"

#define SAMPLE_EVERY_MS 5000
#define WIDTH           SPONZA_RENDER_WIDTH
#define HEIGHT          SPONZA_RENDER_HEIGHT
#define PIXELS          ((size_t)WIDTH * HEIGHT)

enum { BIN_ZERO, BIN_ONE, BIN_TWO_TO_FOUR, BIN_MORE, BIN_COUNT };

typedef struct {
    long drawn;     /* wholly in front, facing, touching the screen */
    long box_empty; /* bounding box holds no pixel centre */
    long bins[BIN_COUNT];
    long box_two_by_two; /* bounding box holds at most 2 x 2 centres */
} sizes_t;

typedef struct {
    float x, y;
} point_t;

static bool
project(const r3d_lit_view_t* view, const int16_t p[3], point_t* out) {
    float l[3];
    for (int r = 0; r < 3; r++) {
        l[r] = (view->m[r][0] * (float)p[0]) + (view->m[r][1] * (float)p[1]) + (view->m[r][2] * (float)p[2])
               + view->m[r][3];
    }
    if (l[2] <= view->near_z) {
        return false;
    }
    *out = (point_t){view->center_x + (l[0] / l[2]), view->center_y + (l[1] / l[2])};
    return true;
}

static float
edge(point_t a, point_t b, float px, float py) {
    return ((b.x - a.x) * (py - a.y)) - ((b.y - a.y) * (px - a.x));
}

/* Top and left edges own their centres; the triangle winds positive. */
static bool
owns(point_t a, point_t b, float px, float py) {
    const float w = edge(a, b, px, py);
    const float dy = b.y - a.y;
    return w > 0.0F || (w == 0.0F && (dy < 0.0F || (dy == 0.0F && b.x > a.x)));
}

static int
first_centre(float lo) {
    return (int)ceilf(lo - 0.5F);
}

typedef struct {
    int x0, x1, y0, y1; /* the centres the bounding box holds, half-open */
} box_t;

static box_t
centre_box(const point_t v[3]) {
    return (box_t){
        first_centre(fminf(v[0].x, fminf(v[1].x, v[2].x))),
        first_centre(fmaxf(v[0].x, fmaxf(v[1].x, v[2].x))),
        first_centre(fminf(v[0].y, fminf(v[1].y, v[2].y))),
        first_centre(fmaxf(v[0].y, fmaxf(v[1].y, v[2].y))),
    };
}

static int
clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Counts the centres a positive-winding triangle owns, stopping past 4. */
static long
count_centres(const point_t v[3], box_t b) {
    long covered = 0;
    for (int y = clamp_int(b.y0, 0, HEIGHT); y < clamp_int(b.y1, 0, HEIGHT) && covered <= 4; y++) {
        for (int x = clamp_int(b.x0, 0, WIDTH); x < clamp_int(b.x1, 0, WIDTH) && covered <= 4; x++) {
            const float px = (float)x + 0.5F;
            const float py = (float)y + 0.5F;
            covered += owns(v[0], v[1], px, py) && owns(v[1], v[2], px, py) && owns(v[2], v[0], px, py);
        }
    }
    return covered;
}

static void
size_triangle(point_t v[3], bool double_sided, sizes_t* s) {
    const float area2 = edge(v[0], v[1], v[2].x, v[2].y);
    if (area2 == 0.0F || (!double_sided && area2 > 0.0F)) {
        return; /* front faces wind negative on screen */
    }
    if (area2 < 0.0F) {
        const point_t t = v[1];
        v[1] = v[2];
        v[2] = t;
    }
    const box_t b = centre_box(v);
    if (b.x1 <= 0 || b.x0 >= WIDTH || b.y1 <= 0 || b.y0 >= HEIGHT) {
        return;
    }
    s->drawn++;
    if (b.x0 >= b.x1 || b.y0 >= b.y1) {
        s->box_empty++;
        s->bins[BIN_ZERO]++;
        return;
    }
    s->box_two_by_two += (b.x1 - b.x0 <= 2 && b.y1 - b.y0 <= 2);
    const long covered = count_centres(v, b);
    s->bins[covered == 0 ? BIN_ZERO : covered == 1 ? BIN_ONE : covered <= 4 ? BIN_TWO_TO_FOUR : BIN_MORE]++;
}

static void
size_pose(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* visible, int count, sizes_t* s) {
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[visible[i]];
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            point_t v[3];
            bool in_front = true;
            for (int k = 0; k < 3; k++) {
                in_front = in_front && project(view, mesh->positions[mesh->triangles[t][k]], &v[k]);
            }
            if (in_front) {
                size_triangle(v, c->double_sided, s);
            }
        }
    }
}

static void
render_pose(const r3d_lit_mesh_t* mesh, const r3d_lit_view_t* view, const uint16_t* visible, int count, void* cs,
            r3d_lit_rows_t* rows, uint16_t* color, uint16_t* depth) {
    memset(color, 0, PIXELS * sizeof(*color));
    memset(depth, 0, PIXELS * sizeof(*depth));
    r3d_lit_transform(mesh, view, visible, count, cs, rows);
    const r3d_span_target_t target = {color, depth, WIDTH, 0, HEIGHT};
    r3d_lit_draw(mesh, view, visible, count, cs, rows, &target);
}

static int
channel_gap(uint16_t a, uint16_t b) {
    const unsigned na = (unsigned)((a >> 8) | (a << 8)) & 0xFFFFU;
    const unsigned nb = (unsigned)((b >> 8) | (b << 8)) & 0xFFFFU;
    const int dr = abs((int)(na >> 11) - (int)(nb >> 11)) * 8;
    const int dg = abs((int)((na >> 5) & 63U) - (int)((nb >> 5) & 63U)) * 4;
    const int db = abs((int)(na & 31U) - (int)(nb & 31U)) * 8;
    return dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
}

/* Writes the frame to dir/pose_NN.raw, or with `compare` reports how it
 * differs from the one already there. */
static void
keep_frame(const char* dir, bool compare, int pose, const uint16_t* color) {
    char path[512];
    snprintf(path, sizeof path, "%s/pose_%02d.raw", dir, pose);
    FILE* f = fopen(path, compare ? "rb" : "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    if (!compare) {
        fwrite(color, sizeof(*color), PIXELS, f);
        fclose(f);
        return;
    }
    uint16_t* before = malloc(PIXELS * sizeof(*before));
    const size_t got = fread(before, sizeof(*before), PIXELS, f);
    fclose(f);
    long differ = 0;
    int worst = 0;
    for (size_t i = 0; i < PIXELS && got == PIXELS; i++) {
        if (before[i] != color[i]) {
            differ++;
            const int gap = channel_gap(before[i], color[i]);
            worst = gap > worst ? gap : worst;
        }
    }
    printf("  pose %2d: %ld of %zu pixels differ (%.3f%%), largest channel gap %d of 255\n", pose, differ, PIXELS,
           100.0 * (double)differ / (double)PIXELS, worst);
    free(before);
}

static void
print_sizes(const char* label, const sizes_t* s) {
    const double n = s->drawn > 0 ? (double)s->drawn : 1.0;
    printf("%-8s drawn %6ld | 0: %5.1f%% (box empty %5.1f%%) | 1: %5.1f%% | 2-4: %5.1f%% | >4: %5.1f%% | box <= 2x2: "
           "%5.1f%%\n",
           label, s->drawn, 100.0 * (double)s->bins[BIN_ZERO] / n, 100.0 * (double)s->box_empty / n,
           100.0 * (double)s->bins[BIN_ONE] / n, 100.0 * (double)s->bins[BIN_TWO_TO_FOUR] / n,
           100.0 * (double)s->bins[BIN_MORE] / n, 100.0 * (double)s->box_two_by_two / n);
}

int
main(int argc, char** argv) {
    const char* dir = NULL;
    bool compare = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        compare = strcmp(argv[i], "--against") == 0;
        dir = argv[i + 1];
    }
    const r3d_lit_mesh_t* mesh = &sponza_mesh;
    uint16_t* visible = malloc(sizeof(uint16_t) * (size_t)mesh->cluster_count);
    r3d_lit_rows_t* rows = malloc(sizeof(r3d_lit_rows_t) * (size_t)mesh->cluster_count);
    void* cs = malloc(sizeof(r3d_lit_vertex_t) * (size_t)mesh->vertex_count);
    uint16_t* color = malloc(PIXELS * sizeof(uint16_t));
    uint16_t* depth = malloc(PIXELS * sizeof(uint16_t));

    printf("Sponza, %d triangles, rendered %dx%d; triangles by pixel centres covered\n", mesh->triangle_count, WIDTH,
           HEIGHT);
    sizes_t total = {0};
    const uint32_t period = r3d_path_period_ms(&sponza_flythrough);
    int pose = 0;
    for (uint32_t t_ms = 0; t_ms < period; t_ms += SAMPLE_EVERY_MS, pose++) {
        r3d_lit_view_t view;
        sponza_view_at(&view, t_ms, mesh->position_scale, 0);
        const int count = r3d_lit_cull_clusters(mesh, &view, visible);
        sizes_t s = {0};
        size_pose(mesh, &view, visible, count, &s);
        char label[16];
        snprintf(label, sizeof label, "t=%us", (unsigned)(t_ms / 1000));
        print_sizes(label, &s);
        total.drawn += s.drawn;
        total.box_empty += s.box_empty;
        total.box_two_by_two += s.box_two_by_two;
        for (int b = 0; b < BIN_COUNT; b++) {
            total.bins[b] += s.bins[b];
        }
        render_pose(mesh, &view, visible, count, cs, rows, color, depth);
        if (dir != NULL) {
            keep_frame(dir, compare, pose, color);
        }
    }
    print_sizes("all", &total);
    free(visible);
    free(rows);
    free(cs);
    free(color);
    free(depth);
    return 0;
}
