/*
 * Portable suite: r3d_span.h's triangle fill, r3d_lit_pipeline.h's camera,
 * culling and near clip, r3d_lit_frame.h's two-core frame, and r3d_path.h's
 * loop. Every mesh here is built inside the test, never a baked one.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_color.h"
#include "render/r3d_lit_frame.h"
#include "render/r3d_lit_pipeline.h"
#include "render/r3d_path.h"
#include "render/r3d_ray.h"

#define W 64
#define H 48

static gfx_color_t* color;
static uint16_t* depth;

static void release_fixture(void);

static r3d_span_target_t
fixture(void) {
    if (color == NULL) {
        color = malloc(sizeof(*color) * W * H);
    }
    if (depth == NULL) {
        depth = malloc(sizeof(*depth) * W * H);
    }
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    suite_set_test_cleanup(release_fixture);
    memset(color, 0, sizeof(*color) * W * H);
    memset(depth, 0, sizeof(*depth) * W * H);
    return (r3d_span_target_t){color, depth, W, 0, H};
}

static r3d_span_vertex_t
sv(float x, float y, float z, float r, float g, float b) {
    return (r3d_span_vertex_t){r3d_span_snap(x), r3d_span_snap(y), z, r, g, b};
}

static int
covered(void) {
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        n += depth[i] != 0;
    }
    return n;
}

/* Span fill */

static void
test_two_triangles_sharing_an_edge_cover_a_square_exactly_once(void) {
    const r3d_span_vertex_t a = sv(10.3f, 7.6f, 0.5f, 255, 0, 0), b = sv(40.7f, 9.2f, 0.5f, 255, 0, 0);
    const r3d_span_vertex_t c = sv(37.1f, 41.4f, 0.5f, 255, 0, 0), d = sv(8.9f, 38.8f, 0.5f, 255, 0, 0);
    uint8_t* hits = malloc(W * H);
    TEST_ASSERT_NOT_NULL(hits);
    memset(hits, 0, W * H);

    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    for (int i = 0; i < W * H; i++) {
        hits[i] += depth[i] != 0;
    }
    t = fixture();
    r3d_span_triangle(&t, &a, &c, &d);
    for (int i = 0; i < W * H; i++) {
        hits[i] += depth[i] != 0;
        TEST_ASSERT_TRUE_MESSAGE(hits[i] <= 1, "a pixel on the shared edge was filled by both triangles");
    }

    /* Nothing inside the quad was missed either: its interior is convex, so
     * a pixel centre strictly between the edges must have been hit once. */
    int filled = 0;
    for (int i = 0; i < W * H; i++) {
        filled += hits[i];
    }
    const float subpixel_area = 0.5f * fabsf((float)((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)))
                                + 0.5f * fabsf((float)((c.x - a.x) * (d.y - a.y) - (d.x - a.x) * (c.y - a.y)));
    const float area = subpixel_area / (float)(R3D_SUBPIXEL * R3D_SUBPIXEL);
    TEST_ASSERT_INT_WITHIN((int)(area * 0.03f), (int)area, filled);
    free(hits);
}

/* A fan of triangles small enough for the flat path, meeting a large one
 * along a shared edge: every pixel inside is filled once, none twice. */
static void
test_tiny_and_large_triangles_tile_without_gaps_or_overlap(void) {
    uint8_t* hits = malloc(W * H);
    TEST_ASSERT_NOT_NULL(hits);
    memset(hits, 0, W * H);
    const float x0 = 5.3f, y0 = 4.7f, cell = 1.37f;
    int drawn = 0;
    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 20; i++) {
            const float x = x0 + (float)i * cell, y = y0 + (float)j * cell;
            const r3d_span_vertex_t p00 = sv(x, y, 0.5f, 90, 90, 90), p10 = sv(x + cell, y, 0.5f, 90, 90, 90);
            const r3d_span_vertex_t p01 = sv(x, y + cell, 0.5f, 90, 90, 90);
            const r3d_span_vertex_t p11 = sv(x + cell, y + cell, 0.5f, 90, 90, 90);
            r3d_span_target_t t = fixture();
            r3d_span_triangle(&t, &p00, &p10, &p11);
            r3d_span_triangle(&t, &p00, &p11, &p01);
            for (int k = 0; k < W * H; k++) {
                hits[k] += depth[k] != 0;
            }
            drawn++;
        }
    }
    /* The large triangle shares the fan's bottom edge. */
    const float bottom = y0 + 12.0f * cell, right = x0 + 20.0f * cell;
    const r3d_span_vertex_t la = sv(x0, bottom, 0.5f, 90, 90, 90), lb = sv(right, bottom, 0.5f, 90, 90, 90);
    const r3d_span_vertex_t lc = sv(x0, 46.0f, 0.5f, 90, 90, 90);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &la, &lb, &lc);
    for (int k = 0; k < W * H; k++) {
        hits[k] += depth[k] != 0;
        TEST_ASSERT_TRUE_MESSAGE(hits[k] <= 1, "a pixel was filled by two triangles");
    }
    for (int y = (int)ceilf(y0 + 0.5f); y + 0.5f < bottom - 0.5f; y++) {
        for (int x = (int)ceilf(x0 + 0.5f); x + 0.5f < right - 0.5f; x++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, hits[y * W + x], "a pixel inside the fan was missed");
        }
    }
    TEST_ASSERT_EQUAL_INT(240, drawn);
    free(hits);
}

static void
test_the_nearer_triangle_wins_in_either_order(void) {
    const r3d_span_vertex_t fa = sv(0, 0, 0.2f, 255, 0, 0), fb = sv(60, 0, 0.2f, 255, 0, 0),
                            fc = sv(0, 45, 0.2f, 255, 0, 0);
    const r3d_span_vertex_t na = sv(0, 0, 0.8f, 0, 0, 255), nb = sv(60, 0, 0.8f, 0, 0, 255),
                            nc = sv(0, 45, 0.8f, 0, 0, 255);

    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &fa, &fb, &fc);
    r3d_span_triangle(&t, &na, &nb, &nc);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), color[5 * W + 5]);

    t = fixture();
    r3d_span_triangle(&t, &na, &nb, &nc);
    r3d_span_triangle(&t, &fa, &fb, &fc);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), color[5 * W + 5]);
}

static void
test_a_window_of_rows_matches_the_same_rows_of_a_full_draw(void) {
    const r3d_span_vertex_t a = sv(3.2f, -20.0f, 0.3f, 10, 200, 30), b = sv(70.0f, 12.5f, 0.9f, 250, 20, 90);
    const r3d_span_vertex_t c = sv(-5.0f, 60.0f, 0.6f, 90, 90, 250);

    r3d_span_target_t full = fixture();
    r3d_span_triangle(&full, &a, &b, &c);
    gfx_color_t* whole = malloc(sizeof(*whole) * W * H);
    TEST_ASSERT_NOT_NULL(whole);
    memcpy(whole, color, sizeof(*whole) * W * H);

    gfx_color_t* band_color = malloc(sizeof(*band_color) * W * 16);
    uint16_t* band_depth = malloc(sizeof(*band_depth) * W * 16);
    TEST_ASSERT_NOT_NULL(band_color);
    TEST_ASSERT_NOT_NULL(band_depth);
    memset(band_color, 0, sizeof(*band_color) * W * 16);
    memset(band_depth, 0, sizeof(*band_depth) * W * 16);
    const r3d_span_target_t band = {band_color, band_depth, W, 16, 32};
    r3d_span_triangle(&band, &a, &b, &c);
    TEST_ASSERT_EQUAL_MEMORY(whole + 16 * W, band_color, sizeof(*band_color) * W * 16);
    free(band_depth);
    free(band_color);
    free(whole);
}

static void
test_colours_stay_within_the_vertex_range_even_at_the_edges(void) {
    const r3d_span_vertex_t a = sv(0.4f, 0.4f, 0.5f, 255, 255, 255), b = sv(63.6f, 1.0f, 0.5f, 255, 255, 255);
    const r3d_span_vertex_t c = sv(2.0f, 47.6f, 0.5f, 0, 0, 0);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    for (int i = 0; i < W * H; i++) {
        if (depth[i] == 0) {
            continue;
        }
        const uint16_t native = (uint16_t)((color[i] >> 8) | (color[i] << 8));
        const int r = native >> 11, g = (native >> 5) & 63, b5 = native & 31;
        /* A wrapped channel would show as a colour whose channels disagree. */
        TEST_ASSERT_INT_WITHIN(1, r, b5);
        TEST_ASSERT_INT_WITHIN(2, r * 2, g);
    }
}

/* Edges on pixel-centre lines: a centre exactly on the left or top edge is
 * inside, one exactly on the right or bottom edge is not. */
static void
test_an_axis_aligned_square_fills_exactly_the_centres_inside_it(void) {
    const r3d_span_vertex_t a = sv(10.5f, 5.5f, 0.5f, 90, 90, 90), b = sv(30.5f, 5.5f, 0.5f, 90, 90, 90);
    const r3d_span_vertex_t c = sv(30.5f, 25.5f, 0.5f, 90, 90, 90), d = sv(10.5f, 25.5f, 0.5f, 90, 90, 90);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    r3d_span_triangle(&t, &a, &c, &d);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const bool inside = x >= 10 && x < 30 && y >= 5 && y < 25;
            TEST_ASSERT_EQUAL_MESSAGE(inside, depth[y * W + x] != 0, "a pixel's centre was judged on the wrong side");
        }
    }
}

/* Small enough for the one-colour path: that colour and depth are the
 * average of the three corners'. */
static void
test_a_tiny_triangle_takes_the_average_of_its_corners(void) {
    const r3d_span_vertex_t a = sv(20.2f, 10.2f, 0.2f, 255, 0, 0), b = sv(22.8f, 10.2f, 0.5f, 0, 255, 0);
    const r3d_span_vertex_t c = sv(21.5f, 11.9f, 0.8f, 0, 0, 255);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    TEST_ASSERT_GREATER_THAN_INT(0, covered());
    for (int i = 0; i < W * H; i++) {
        if (depth[i] != 0) {
            TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x555555), color[i]);
            TEST_ASSERT_UINT16_WITHIN(1, 32767, depth[i]);
        }
    }
}

/* Slivers whose depth falls steeply along a span: extrapolated to the
 * span's last pixel, the depth leaves its range, and a wrapped value would
 * put that pixel in front of everything. */
static void
test_a_steep_sliver_puts_no_pixel_nearer_than_its_nearest_corner(void) {
    static const float slivers[][3][6] = {
        {{24.3595695f, 36.5131073f, 0.548884869f, 0, 0, 0},
         {68.97229f, 47.5016022f, 0.159555957f, 255, 255, 255},
         {68.3385162f, 48.167408f, 0.398059934f, 0, 0, 0}},
        {{13.4072399f, 38.8770714f, 0.0348051414f, 255, 255, 255},
         {36.4059906f, 54.54039f, 0.211613521f, 0, 0, 0},
         {-2.3143692f, 29.7081623f, 0.675357819f, 0, 0, 0}},
    };
    for (int i = 0; i < 2; i++) {
        r3d_span_vertex_t v[3];
        for (int k = 0; k < 3; k++) {
            const float* f = slivers[i][k];
            v[k] = sv(f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        const float nearest = fmaxf(v[0].z, fmaxf(v[1].z, v[2].z));
        r3d_span_target_t t = fixture();
        r3d_span_triangle(&t, &v[0], &v[1], &v[2]);
        for (int p = 0; p < W * H; p++) {
            TEST_ASSERT_TRUE_MESSAGE(depth[p] <= nearest * 65535.0f + 256.0f,
                                     "a pixel came out nearer than any corner");
        }
    }
}

static uint32_t
next_random(uint32_t* state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state >> 8;
}

/* A subpixel position in [lo, lo + span), landing on a pixel centre or a
 * pixel edge one time in four so the ties are exercised. */
static int32_t
random_subpixel(uint32_t* state, float lo, float span) {
    const int32_t v = r3d_span_snap(lo + (span * (float)(next_random(state) % 10000u) / 10000.0f));
    return next_random(state) % 4u == 0 ? v & ~7 : v;
}

/* The textbook rule, written apart from r3d_span: with the triangle turned
 * to wind positive, a centre is inside when it is inside every edge, or on
 * one that is a top or a left edge. */
static bool
reference_inside(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, int x, int y) {
    const int64_t px = ((int64_t)x * R3D_SUBPIXEL) + (R3D_SUBPIXEL / 2);
    const int64_t py = ((int64_t)y * R3D_SUBPIXEL) + (R3D_SUBPIXEL / 2);
    const r3d_span_vertex_t* v[3] = {a, b, c};
    const int64_t area =
        (((int64_t)b->x - a->x) * ((int64_t)c->y - a->y)) - (((int64_t)b->y - a->y) * ((int64_t)c->x - a->x));
    if (area == 0) {
        return false;
    }
    if (area < 0) {
        v[1] = c;
        v[2] = b;
    }
    for (int k = 0; k < 3; k++) {
        const r3d_span_vertex_t* p = v[k];
        const r3d_span_vertex_t* q = v[(k + 1) % 3];
        const int64_t dx = (int64_t)q->x - p->x;
        const int64_t dy = (int64_t)q->y - p->y;
        const int64_t w = (dx * (py - p->y)) - (dy * (px - p->x));
        const bool top_left = dy < 0 || (dy == 0 && dx > 0);
        if (w < 0 || (w == 0 && !top_left)) {
            return false;
        }
    }
    return true;
}

/* Triangles from a fraction of a pixel to forty, so both the centre-by-
 * centre path and the row walk are taken, each drawn as two windows of rows:
 * every pixel is covered exactly when the reference says so. */
static void
test_every_triangle_covers_exactly_the_centres_the_top_left_rule_gives(void) {
    uint32_t state = 12345u;
    int mismatches = 0;
    int split_differs = 0;
    gfx_color_t* whole_color = malloc(sizeof(*whole_color) * W * H);
    uint16_t* whole_depth = malloc(sizeof(*whole_depth) * W * H);
    TEST_ASSERT_NOT_NULL(whole_color);
    TEST_ASSERT_NOT_NULL(whole_depth);
    for (int i = 0; i < 2000; i++) {
        const float size = 0.3f * powf(2.0f, (float)(next_random(&state) % 800u) / 100.0f);
        const float cx = -4.0f + (float)(next_random(&state) % (unsigned)(W + 8));
        const float cy = -4.0f + (float)(next_random(&state) % (unsigned)(H + 8));
        r3d_span_vertex_t v[3];
        for (int k = 0; k < 3; k++) {
            v[k] = sv(0, 0, 0.5f, 90, 90, 90);
            v[k].x = random_subpixel(&state, cx - size, 2.0f * size);
            v[k].y = random_subpixel(&state, cy - size, 2.0f * size);
        }
        const int split = (int)(next_random(&state) % (unsigned)(H + 1));
        r3d_span_target_t t = fixture();
        r3d_span_triangle(&t, &v[0], &v[1], &v[2]);
        memcpy(whole_color, color, sizeof(*color) * W * H);
        memcpy(whole_depth, depth, sizeof(*depth) * W * H);
        t = fixture();
        const r3d_span_target_t top = {t.color, t.depth, W, 0, split};
        const r3d_span_target_t bottom = {t.color + (split * W), t.depth + (split * W), W, split, H};
        r3d_span_triangle(&top, &v[0], &v[1], &v[2]);
        r3d_span_triangle(&bottom, &v[0], &v[1], &v[2]);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const int p = (y * W) + x;
                mismatches += (depth[p] != 0) != reference_inside(&v[0], &v[1], &v[2], x, y);
                split_differs += depth[p] != whole_depth[p] || color[p] != whole_color[p];
            }
        }
    }
    free(whole_color);
    free(whole_depth);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, split_differs, "a window of rows drew a pixel unlike the whole draw");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches, "a pixel's coverage disagrees with the top-left rule");
}

#define GRID_LINES_MAX 160

/* Grid lines on half pixels from -3 to hi + 3, half a pixel to six apart,
 * mostly under two. */
static int
grid_lines(uint32_t* state, float hi, float* out) {
    int n = 0;
    float v = -3.0f;
    while (n < GRID_LINES_MAX - 1 && v < hi + 3.0f) {
        out[n++] = v;
        const uint32_t r = next_random(state);
        v += 0.5f * (float)(1 + (r % 4u == 0 ? r % 12u : r % 3u));
    }
    out[n++] = v; /* the first line past hi + 3, so the grid covers it */
    return n;
}

/* Half the corners stay on their grid lines; the rest move by up to a
 * fifth of the narrowest cell, so every cell stays convex. */
static void
jittered_grid(uint32_t* state, const float* xs, int nx, const float* ys, int ny, r3d_span_vertex_t* grid) {
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            const float jitter = next_random(state) % 2u == 0 ? 0.0f : 0.2f * 0.5f;
            const float jx = jitter * (((float)(next_random(state) % 200u) / 100.0f) - 1.0f);
            const float jy = jitter * (((float)(next_random(state) % 200u) / 100.0f) - 1.0f);
            grid[(j * nx) + i] = sv(xs[i] + jx, ys[j] + jy, 0.5f, 90, 90, 90);
        }
    }
}

/* Draws one triangle alone and adds its pixels to hits; returns true when
 * its bounding box holds at most 2 x 2 centres and it covered one. */
static bool
draw_counting(const r3d_span_vertex_t* const tri[3], uint8_t* hits) {
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, tri[0], tri[1], tri[2]);
    for (int p = 0; p < W * H; p++) {
        hits[p] += depth[p] != 0;
    }
    const int columns = r3d_span_first_centre(r3d_span_max3(tri[0]->x, tri[1]->x, tri[2]->x))
                        - r3d_span_first_centre(r3d_span_min3(tri[0]->x, tri[1]->x, tri[2]->x));
    const int rows = r3d_span_first_centre(r3d_span_max3(tri[0]->y, tri[1]->y, tri[2]->y))
                     - r3d_span_first_centre(r3d_span_min3(tri[0]->y, tri[1]->y, tri[2]->y));
    return columns <= 2 && rows <= 2 && covered() > 0;
}

/* A mesh whose cells run from half a pixel to six, so triangles tested
 * centre by centre share edges with walked ones, and whose unjittered
 * corners sit on half pixels, so edges run through centres: every pixel is
 * filled by exactly one triangle. */
static void
test_a_mesh_of_small_and_large_triangles_fills_every_pixel_exactly_once(void) {
    uint32_t state = 777u;
    float* xs = malloc(sizeof(float) * GRID_LINES_MAX);
    float* ys = malloc(sizeof(float) * GRID_LINES_MAX);
    TEST_ASSERT_NOT_NULL(xs);
    TEST_ASSERT_NOT_NULL(ys);
    const int nx = grid_lines(&state, (float)W, xs);
    const int ny = grid_lines(&state, (float)H, ys);
    r3d_span_vertex_t* grid = malloc(sizeof(*grid) * (size_t)(nx * ny));
    uint8_t* hits = calloc(W * H, 1);
    TEST_ASSERT_NOT_NULL(grid);
    TEST_ASSERT_NOT_NULL(hits);
    jittered_grid(&state, xs, nx, ys, ny, grid);
    int small = 0;
    for (int j = 0; j + 1 < ny; j++) {
        for (int i = 0; i + 1 < nx; i++) {
            const r3d_span_vertex_t* p00 = &grid[(j * nx) + i];
            const r3d_span_vertex_t* p01 = p00 + nx;
            const bool other_diagonal = next_random(&state) % 2u == 0;
            const r3d_span_vertex_t* const first[3] = {p00, p00 + 1, other_diagonal ? p01 : p01 + 1};
            const r3d_span_vertex_t* const second[3] = {other_diagonal ? p00 + 1 : p00, p01 + 1, p01};
            small += draw_counting(first, hits);
            small += draw_counting(second, hits);
        }
    }
    for (int p = 0; p < W * H; p++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, hits[p], "a pixel was missed or filled twice");
    }
    TEST_ASSERT_GREATER_THAN_INT(100, small);
    free(grid);
    free(hits);
    free(xs);
    free(ys);
}

/* A tall sliver at most two centres wide, cut so one window holds one or
 * two of its rows: each window draws exactly the whole triangle's pixels,
 * colour and depth, whatever path its full height takes. */
static void
test_a_window_holding_a_few_rows_of_a_tall_sliver_draws_them_as_the_whole_does(void) {
    const r3d_span_vertex_t a = sv(10.4f, 0.2f, 0.2f, 255, 0, 0), b = sv(12.4f, 0.2f, 0.9f, 0, 255, 0);
    const r3d_span_vertex_t c = sv(11.4f, 20.0f, 0.5f, 0, 0, 255);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    gfx_color_t* whole_color = malloc(sizeof(*whole_color) * W * H);
    uint16_t* whole_depth = malloc(sizeof(*whole_depth) * W * H);
    TEST_ASSERT_NOT_NULL(whole_color);
    TEST_ASSERT_NOT_NULL(whole_depth);
    memcpy(whole_color, color, sizeof(*color) * W * H);
    memcpy(whole_depth, depth, sizeof(*depth) * W * H);
    for (int split = 1; split <= 3; split++) {
        t = fixture();
        const r3d_span_target_t top = {t.color, t.depth, W, 0, split};
        const r3d_span_target_t bottom = {t.color + (split * W), t.depth + (split * W), W, split, H};
        r3d_span_triangle(&top, &a, &b, &c);
        r3d_span_triangle(&bottom, &a, &b, &c);
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(whole_depth, depth, W * H, "a window changed a pixel's depth");
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(whole_color, color, W * H, "a window changed a pixel's colour");
    }
    free(whole_color);
    free(whole_depth);
}

/* A lit mesh seen through a view whose screen position is a vertex's own
 * x and y in subpixels over its z in ticks, centred on the origin. */
typedef struct {
    int16_t positions[9][3];
    uint8_t colors[9][3];
    uint16_t triangles[8][3];
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
} screen_mesh_t;

static r3d_lit_view_t
subpixel_view(void) {
    const float s = 1.0f / (float)R3D_SUBPIXEL;
    return (r3d_lit_view_t){{{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, 1, 0}},
                            0.0f,
                            0.0f,
                            0.5f,
                            W,
                            H,
                            R3D_SNAP_BIAS,
                            R3D_SNAP_BIAS,
                            0.5f / (float)R3D_SUBPIXEL};
}

static r3d_lit_mesh_t
screen_mesh(screen_mesh_t* m, int vertices, int triangles) {
    for (int v = 0; v < vertices; v++) {
        m->colors[v][0] = m->colors[v][1] = m->colors[v][2] = 200;
    }
    m->cluster = (r3d_lit_cluster_t){
        0, (uint16_t)vertices, 0, (uint16_t)triangles, {-32767, -32767, -8}, {32767, 32767, 8}, true};
    m->node = (r3d_lit_node_t){{-32767, -32767, -8}, {32767, 32767, 8}, 0, 1, true};
    return (r3d_lit_mesh_t){m->positions, m->colors, m->triangles, &m->cluster, &m->node, vertices, triangles, 1, 1, 1,
                            NULL,         NULL};
}

/* Transforms the mesh with cluster rows and draws it as two windows. */
static void
draw_screen_mesh(const r3d_lit_mesh_t* mesh, int split) {
    const r3d_lit_view_t view = subpixel_view();
    const uint16_t visible[1] = {0};
    r3d_lit_vertex_t cs[9];
    r3d_lit_rows_t rows[1];
    r3d_lit_transform(mesh, &view, visible, 1, cs, rows);
    r3d_span_target_t t = fixture();
    const r3d_span_target_t top = {t.color, t.depth, W, 0, split};
    const r3d_span_target_t bottom = {t.color + (split * W), t.depth + (split * W), W, split, H};
    r3d_lit_draw(mesh, &view, visible, 1, cs, rows, &top);
    r3d_lit_draw(mesh, &view, visible, 1, cs, rows, &bottom);
}

/* Whether the reference says the same of a centre nudged a subpixel either
 * way: a centre that close to an edge moves with a clip's rounding. */
static bool
robustly(const r3d_span_vertex_t v[3], int x, int y, bool* inside) {
    *inside = reference_inside(&v[0], &v[1], &v[2], x, y);
    for (int k = 0; k < 4; k++) {
        r3d_span_vertex_t moved[3] = {v[0], v[1], v[2]};
        for (int i = 0; i < 3; i++) {
            moved[i].x += (k == 0) - (k == 1);
            moved[i].y += (k == 2) - (k == 3);
        }
        if (reference_inside(&moved[0], &moved[1], &moved[2], x, y) != *inside) {
            return false;
        }
    }
    return true;
}

/* One, two, then all three corners in front but too far off screen to
 * snap: the triangle is rebuilt and clipped, and still fills the centres
 * it covers, drawn through cluster rows into two windows. */
static void
test_a_triangle_with_corners_past_the_snap_range_fills_its_centres(void) {
    static const int16_t near_corners[3][2] = {{85, 66}, {971, 158}, {323, 713}};
    static const int16_t far_corners[3][2] = {{-30400, -28800}, {31200, 485}, {254, 31800}};
    for (int far = 1; far <= 3; far++) {
        screen_mesh_t m;
        r3d_span_vertex_t reference[3];
        for (int k = 0; k < 3; k++) {
            const int16_t* xy = k < far ? far_corners[k] : near_corners[k];
            m.positions[k][0] = xy[0];
            m.positions[k][1] = xy[1];
            m.positions[k][2] = 1;
            reference[k] = sv(0, 0, 0.5f, 0, 0, 0);
            reference[k].x = xy[0];
            reference[k].y = xy[1];
        }
        m.triangles[0][0] = 0;
        m.triangles[0][1] = 1;
        m.triangles[0][2] = 2;
        const r3d_lit_mesh_t mesh = screen_mesh(&m, 3, 1);
        draw_screen_mesh(&mesh, H / 2 + 1);
        int filled = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                bool inside;
                if (robustly(reference, x, y, &inside)) {
                    TEST_ASSERT_EQUAL_MESSAGE(inside, depth[(y * W) + x] != 0,
                                              "a far-cornered triangle's pixel is wrong");
                }
                filled += depth[(y * W) + x] != 0;
            }
        }
        TEST_ASSERT_GREATER_THAN_INT(0, filled);
    }
}

/* A triangle around a single centre at each edge of a window, drawn through
 * r3d_lit_draw: the cull that drops triangles holding no centre keeps it. */
static void
test_a_triangle_whose_only_centre_is_at_a_window_edge_is_drawn(void) {
    const int row0 = 10;
    const int row1 = 30;
    const int spots[4][2] = {{0, 15}, {W - 1, 15}, {20, row0}, {20, row1 - 1}};
    for (int i = 0; i < 4; i++) {
        const int cx = (spots[i][0] * R3D_SUBPIXEL) + (R3D_SUBPIXEL / 2);
        const int cy = (spots[i][1] * R3D_SUBPIXEL) + (R3D_SUBPIXEL / 2);
        screen_mesh_t m;
        const int16_t corners[3][2] = {{(int16_t)(cx - 3), (int16_t)(cy - 3)},
                                       {(int16_t)(cx + 4), (int16_t)(cy - 3)},
                                       {(int16_t)cx, (int16_t)(cy + 4)}};
        for (int k = 0; k < 3; k++) {
            m.positions[k][0] = corners[k][0];
            m.positions[k][1] = corners[k][1];
            m.positions[k][2] = 1;
            m.triangles[0][k] = (uint16_t)k;
        }
        const r3d_lit_mesh_t mesh = screen_mesh(&m, 3, 1);
        const r3d_lit_view_t view = subpixel_view();
        const uint16_t visible[1] = {0};
        r3d_lit_vertex_t cs[3];
        r3d_lit_rows_t rows[1];
        r3d_lit_transform(&mesh, &view, visible, 1, cs, rows);
        r3d_span_target_t t = fixture();
        const r3d_span_target_t window = {t.color + (row0 * W), t.depth + (row0 * W), W, row0, row1};
        r3d_lit_draw(&mesh, &view, visible, 1, cs, rows, &window);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, covered(), "a one-centre triangle at a window edge was dropped");
        TEST_ASSERT_NOT_EQUAL(0, depth[(spots[i][1] * W) + spots[i][0]]);
    }
}

/* A fan whose outer corners sit just inside and just past the fast path's
 * reach, so rebuilt, clipped triangles share edges across the screen with
 * fast ones: each drawn alone, together they fill every pixel once. */
static void
test_rebuilt_and_fast_triangles_sharing_edges_fill_every_pixel_once(void) {
    static const float radius[8] = {900.0f, 950.0f, 1500.0f, 980.0f, 991.9f, 992.1f, 1900.0f, 991.0f};
    screen_mesh_t m;
    const int centre_x = 32 * R3D_SUBPIXEL + 5;
    const int centre_y = 24 * R3D_SUBPIXEL + 3;
    m.positions[8][0] = (int16_t)centre_x;
    m.positions[8][1] = (int16_t)centre_y;
    m.positions[8][2] = 1;
    for (int k = 0; k < 8; k++) {
        const float angle = 0.785398f * (float)k + 0.3f;
        m.positions[k][0] = (int16_t)((float)centre_x + (radius[k] * (float)R3D_SUBPIXEL * cosf(angle)));
        m.positions[k][1] = (int16_t)((float)centre_y + (radius[k] * (float)R3D_SUBPIXEL * sinf(angle)));
        m.positions[k][2] = 1;
        m.triangles[k][0] = 8;
        m.triangles[k][1] = (uint16_t)k;
        m.triangles[k][2] = (uint16_t)((k + 1) % 8);
    }
    r3d_lit_mesh_t mesh = screen_mesh(&m, 9, 8);
    uint8_t* hits = calloc(W * H, 1);
    TEST_ASSERT_NOT_NULL(hits);
    for (int k = 0; k < 8; k++) {
        m.cluster.triangle_first = (uint16_t)k;
        m.cluster.triangle_count = 1;
        draw_screen_mesh(&mesh, H / 2);
        for (int p = 0; p < W * H; p++) {
            hits[p] += depth[p] != 0;
        }
    }
    for (int p = 0; p < W * H; p++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, hits[p], "a pixel was missed or filled twice along a shared edge");
    }
    free(hits);
}

/* Camera and pipeline */

static const int16_t quad_positions[][3] = {{-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {-100, 100, 0}};
static const uint8_t quad_colors[][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
static const uint16_t quad_front[][3] = {{0, 1, 2}, {0, 2, 3}};
static const uint16_t quad_back[][3] = {{0, 2, 1}, {0, 3, 2}};

static const r3d_lit_node_t quad_node = {{-100, -100, 0}, {100, 100, 0}, 0, 1, true};

static r3d_lit_mesh_t
quad_mesh(const uint16_t (*triangles)[3], bool double_sided, r3d_lit_cluster_t* cluster) {
    *cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {-100, -100, 0}, {100, 100, 0}, double_sided};
    return (r3d_lit_mesh_t){quad_positions, quad_colors, triangles, cluster, &quad_node, 4, 2, 1, 1, 1, NULL, NULL};
}

/* The quad faces +z; this camera stands on +z looking back at it. */
static int
draw_quad(const uint16_t (*triangles)[3], bool double_sided) {
    r3d_lit_cluster_t cluster;
    const r3d_lit_mesh_t mesh = quad_mesh(triangles, double_sided, &cluster);
    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, 0, 400}, (r3d_vec3f_t){0, 0, -1}, 0.5f, 1.0f, 1,
                      (r3d_viewport_t){W, H, 0});
    uint16_t visible[1];
    const int count = r3d_lit_cull_clusters(&mesh, &view, visible);
    r3d_lit_vertex_t cs[4];
    r3d_lit_transform(&mesh, &view, visible, count, cs, NULL);
    const r3d_span_target_t t = fixture();
    r3d_lit_draw(&mesh, &view, visible, count, cs, NULL, &t);
    return covered();
}

/* 200 units across at 400 away, 48 pixels per unit of tangent: 24 pixels
 * square about the centre. */
static void
test_a_counter_clockwise_face_toward_the_camera_is_drawn(void) {
    TEST_ASSERT_EQUAL_INT(24 * 24, draw_quad(quad_front, false));
    for (int y = 12; y < 36; y++) {
        for (int x = 20; x < 44; x++) {
            TEST_ASSERT_NOT_EQUAL(0, depth[y * W + x]);
        }
    }
}

static void
test_a_face_turned_away_is_culled_unless_double_sided(void) {
    TEST_ASSERT_EQUAL_INT(0, draw_quad(quad_back, false));
    TEST_ASSERT_EQUAL_INT(draw_quad(quad_front, false), draw_quad(quad_back, true));
}

static void
test_a_cluster_behind_the_camera_is_culled(void) {
    r3d_lit_cluster_t cluster;
    const r3d_lit_mesh_t mesh = quad_mesh(quad_front, false, &cluster);
    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, 0, 400}, (r3d_vec3f_t){0, 0, 1}, 0.5f, 1.0f, 1,
                      (r3d_viewport_t){W, H, 0});
    uint16_t visible[1];
    TEST_ASSERT_EQUAL_INT(0, r3d_lit_cull_clusters(&mesh, &view, visible));
}

/* A floor running from behind the camera to far ahead: the near clip keeps
 * the part in front and nothing lands above the horizon row. */
static void
test_a_floor_crossing_the_near_plane_draws_only_below_the_horizon(void) {
    static const int16_t floor_positions[][3] = {
        {-1000, 0, 1000}, {1000, 0, 1000}, {1000, 0, -3000}, {-1000, 0, -3000}};
    static const uint16_t floor_up[][3] = {{0, 1, 2}, {0, 2, 3}};
    r3d_lit_cluster_t cluster = {0, 4, 0, 2, {-1000, 0, -3000}, {1000, 0, 1000}, false};
    static const r3d_lit_node_t floor_node = {{-1000, 0, -3000}, {1000, 0, 1000}, 0, 1, true};
    const r3d_lit_mesh_t mesh = {
        floor_positions, quad_colors, floor_up, &cluster, &floor_node, 4, 2, 1, 1, 1, NULL, NULL};

    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, 50, 0}, (r3d_vec3f_t){0, 0, -1}, 0.5f, 1.0f, 1,
                      (r3d_viewport_t){W, H, 0});
    uint16_t visible[1];
    const int count = r3d_lit_cull_clusters(&mesh, &view, visible);
    TEST_ASSERT_EQUAL_INT(1, count);
    r3d_lit_vertex_t cs[4];
    r3d_lit_transform(&mesh, &view, visible, count, cs, NULL);
    const r3d_span_target_t t = fixture();
    r3d_lit_draw(&mesh, &view, visible, count, cs, NULL, &t);

    for (int y = 0; y < H / 2; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_UINT16(0, depth[y * W + x]);
        }
    }
    /* The floor's far edge sits at row 24.8 and it is wider than the view
     * from row 26 down. */
    for (int y = 26; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, depth[y * W + x], "a pixel of the floor was missed");
        }
    }
}

/* A point up and to the right of the view axis lands up and to the right in
 * the upright picture, whichever way the panel is turned. */
static void
test_a_point_up_and_right_lands_up_and_right_in_every_quarter(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        const r3d_viewport_t viewport = {W, H, quarter};
        r3d_lit_view_t view;
        r3d_lit_view_look(&view, (r3d_vec3f_t){0, 0, 0}, (r3d_vec3f_t){0, 0, -1}, 0.5f, 1.0f, 1, viewport);
        const float px = view.m[0][0] * 30 + view.m[0][1] * 20 + view.m[0][2] * -100 + view.m[0][3];
        const float py = view.m[1][0] * 30 + view.m[1][1] * 20 + view.m[1][2] * -100 + view.m[1][3];
        const float pz = view.m[2][0] * 30 + view.m[2][1] * 20 + view.m[2][2] * -100 + view.m[2][3];
        int ux, uy;
        r3d_physical_to_upright(viewport, (int)floorf(view.center_x + px / pz), (int)floorf(view.center_y + py / pz),
                                &ux, &uy);

        /* The lens fits the shorter upright axis: tan = 0.5 spans half of it. */
        const int upright_width = (quarter & 1) ? H : W, upright_height = (quarter & 1) ? W : H;
        const float per_unit = (float)(W < H ? W : H) / (2.0f * 0.5f) / 100.0f;
        TEST_ASSERT_INT_WITHIN(1, (int)((float)upright_width / 2.0f + 30.0f * per_unit), ux);
        TEST_ASSERT_INT_WITHIN(1, (int)((float)upright_height / 2.0f - 20.0f * per_unit), uy);
    }
}

/* A small mesh assembled in a test, one cluster per part, every cluster
 * under a single leaf. */
#define PARTS_MAX 12

typedef struct {
    int16_t positions[PARTS_MAX * 4][3];
    uint8_t colors[PARTS_MAX * 4][3];
    uint16_t triangles[PARTS_MAX * 2][3];
    r3d_lit_cluster_t clusters[PARTS_MAX];
    r3d_lit_node_t node;
    r3d_lit_mesh_t mesh;
} parts_t;

static parts_t* shared_parts;

static parts_t*
parts_buffer(void) {
    if (shared_parts == NULL) {
        shared_parts = malloc(sizeof(*shared_parts));
    }
    TEST_ASSERT_NOT_NULL(shared_parts);
    suite_set_test_cleanup(release_fixture);
    return shared_parts;
}

static void
parts_begin(parts_t* p) {
    memset(p, 0, sizeof *p);
    p->mesh = (r3d_lit_mesh_t){p->positions, p->colors, p->triangles, p->clusters, &p->node, 0, 0, 0, 1, 1, NULL, NULL};
}

static void
widen(int16_t lo[3], int16_t hi[3], const int16_t p[3]) {
    for (int k = 0; k < 3; k++) {
        lo[k] = p[k] < lo[k] ? p[k] : lo[k];
        hi[k] = p[k] > hi[k] ? p[k] : hi[k];
    }
}

/* A fan of n = 3 or 4 corners, counter-clockwise seen from the side that
 * faces. */
static void
parts_add(parts_t* p, const int16_t corners[][3], int n, const uint8_t rgb[][3], bool double_sided) {
    r3d_lit_mesh_t* m = &p->mesh;
    TEST_ASSERT_TRUE(m->cluster_count < PARTS_MAX && (n == 3 || n == 4));
    const uint16_t first = (uint16_t)m->vertex_count;
    r3d_lit_cluster_t* c = &p->clusters[m->cluster_count++];
    *c = (r3d_lit_cluster_t){first,
                             (uint16_t)n,
                             (uint16_t)m->triangle_count,
                             (uint16_t)(n - 2),
                             {INT16_MAX, INT16_MAX, INT16_MAX},
                             {INT16_MIN, INT16_MIN, INT16_MIN},
                             double_sided};
    for (int i = 0; i < n; i++) {
        memcpy(p->positions[first + i], corners[i], sizeof p->positions[0]);
        memcpy(p->colors[first + i], rgb[i], sizeof p->colors[0]);
        widen(c->lo, c->hi, corners[i]);
    }
    for (int t = 0; t < n - 2; t++) {
        uint16_t* tri = p->triangles[m->triangle_count++];
        tri[0] = first;
        tri[1] = (uint16_t)(first + t + 1);
        tri[2] = (uint16_t)(first + t + 2);
    }
    m->vertex_count += n;

    p->node = (r3d_lit_node_t){
        {INT16_MAX, INT16_MAX, INT16_MAX}, {INT16_MIN, INT16_MIN, INT16_MIN}, 0, (uint8_t)m->cluster_count, true};
    for (int i = 0; i < m->cluster_count; i++) {
        widen(p->node.lo, p->node.hi, p->clusters[i].lo);
        widen(p->node.lo, p->node.hi, p->clusters[i].hi);
    }
}

/* A rectangle in the plane z, facing +z. */
static void
parts_add_rect(parts_t* p, int x0, int y0, int x1, int y1, int z, uint8_t shade) {
    const int16_t corners[4][3] = {{(int16_t)x0, (int16_t)y0, (int16_t)z},
                                   {(int16_t)x1, (int16_t)y0, (int16_t)z},
                                   {(int16_t)x1, (int16_t)y1, (int16_t)z},
                                   {(int16_t)x0, (int16_t)y1, (int16_t)z}};
    const uint8_t rgb[4][3] = {{shade, 40, 200}, {shade, 90, 150}, {shade, 140, 100}, {shade, 190, 50}};
    parts_add(p, corners, 4, rgb, false);
}

static r3d_lit_view_t
look_down_minus_z(float eye_y, float eye_z, float near_z) {
    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, eye_y, eye_z}, (r3d_vec3f_t){0, 0, -1}, 0.5f, near_z, 1,
                      (r3d_viewport_t){W, H, 0});
    return view;
}

/* Cull, transform and draw every cluster into `t`; with `use_rows`, the
 * draw skips clusters by their rows. */
static void
draw_parts(const parts_t* p, const r3d_lit_view_t* view, const r3d_span_target_t* t, bool use_rows) {
    uint16_t* visible = malloc(sizeof(*visible) * PARTS_MAX);
    r3d_lit_vertex_t* cs = malloc(sizeof(*cs) * PARTS_MAX * 4);
    r3d_lit_rows_t* rows = malloc(sizeof(*rows) * PARTS_MAX);
    TEST_ASSERT_NOT_NULL(visible);
    TEST_ASSERT_NOT_NULL(cs);
    TEST_ASSERT_NOT_NULL(rows);
    const int count = r3d_lit_cull_clusters(&p->mesh, view, visible);
    r3d_lit_transform(&p->mesh, view, visible, count, cs, use_rows ? rows : NULL);
    r3d_lit_draw(&p->mesh, view, visible, count, cs, use_rows ? rows : NULL, t);
    free(rows);
    free(cs);
    free(visible);
}

/* One corner 50 away, in front of the camera but behind a near plane at
 * 100; the other two 300 away. The near plane cuts the triangle into a
 * quad whose corners are all on screen. */
static const int16_t cut_corners[3][3] = {{0, 60, -50}, {-80, -40, -300}, {80, -40, -300}};
static const int16_t cut_corners_reversed[3][3] = {{0, 60, -50}, {80, -40, -300}, {-80, -40, -300}};
static const uint8_t cut_colors[3][3] = {{0, 128, 128}, {255, 128, 128}, {255, 128, 128}};

static int
draw_cut(const int16_t corners[3][3], bool double_sided) {
    parts_t* const p = parts_buffer();
    parts_begin(p);
    parts_add(p, corners, 3, cut_colors, double_sided);
    const r3d_lit_view_t view = look_down_minus_z(0, 0, 100.0f);
    const r3d_span_target_t t = fixture();
    draw_parts(p, &view, &t, false);
    return covered();
}

/* The cut corners land at (24.32, 4.8) and (39.68, 4.8), the far ones at
 * (19.2, 30.4) and (44.8, 30.4): a trapezoid of 524 pixels. */
static void
test_a_triangle_cut_by_the_near_plane_draws_the_whole_quad_left_in_front(void) {
    TEST_ASSERT_INT_WITHIN(21, 524, draw_cut(cut_corners, false));
}

/* Where the plane cuts, the colour is a fifth of the way from the near
 * corner's to the far ones': red 51 there, rising to 255 at the far edge. */
static void
test_colour_along_the_near_cut_is_interpolated_to_the_cut(void) {
    draw_cut(cut_corners, false);
    int first_row = -1;
    for (int i = 0; i < W * H && first_row < 0; i++) {
        first_row = depth[i] != 0 ? i / W : -1;
    }
    TEST_ASSERT_TRUE(first_row >= 0);
    for (int x = 0; x < W; x++) {
        const int i = first_row * W + x;
        if (depth[i] != 0) {
            const uint16_t native = (uint16_t)((color[i] >> 8) | (color[i] << 8));
            TEST_ASSERT_INT_WITHIN_MESSAGE(2, 51 >> 3, native >> 11, "the cut edge does not carry the cut's colour");
        }
    }
}

static void
test_a_double_sided_triangle_cut_by_the_near_plane_draws_from_behind(void) {
    const int front = draw_cut(cut_corners, false);
    TEST_ASSERT_EQUAL_INT(0, draw_cut(cut_corners_reversed, false));
    TEST_ASSERT_EQUAL_INT(front, draw_cut(cut_corners_reversed, true));
}

/* Rectangles at several heights over a floor that reaches behind the
 * camera, so one cluster has no bounded rows. */
static void
build_floor_and_rects(parts_t* p) {
    parts_begin(p);
    static const int16_t floor_corners[4][3] = {{-1000, 0, 1000}, {1000, 0, 1000}, {1000, 0, -3000}, {-1000, 0, -3000}};
    static const uint8_t floor_rgb[4][3] = {{200, 200, 200}, {200, 200, 200}, {60, 60, 60}, {60, 60, 60}};
    parts_add(p, floor_corners, 4, floor_rgb, false);
    parts_add_rect(p, -20, 83, 20, 98, -100, 250);
    parts_add_rect(p, -30, 58, 0, 73, -100, 180);
    parts_add_rect(p, 0, 28, 30, 43, -100, 120);
    parts_add_rect(p, -20, 3, 20, 18, -100, 60);
}

static void
test_drawing_a_window_with_cluster_rows_matches_a_full_draw(void) {
    parts_t* const p = parts_buffer();
    build_floor_and_rects(p);
    const r3d_lit_view_t view = look_down_minus_z(50, 0, 1.0f);
    const r3d_span_target_t full = fixture();
    draw_parts(p, &view, &full, false);

    gfx_color_t* band_color = malloc(sizeof(gfx_color_t) * W * H);
    uint16_t* band_depth = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(band_color);
    TEST_ASSERT_NOT_NULL(band_depth);
    static const int windows[][2] = {{0, 8}, {8, 16}, {16, 24}, {24, 32}, {32, 40}, {40, 48}, {5, 13}, {29, 43}};
    for (int w = 0; w < (int)(sizeof windows / sizeof windows[0]); w++) {
        const int row0 = windows[w][0], row1 = windows[w][1];
        memset(band_color, 0, sizeof(gfx_color_t) * W * H);
        memset(band_depth, 0, sizeof(uint16_t) * W * H);
        const r3d_span_target_t band = {band_color, band_depth, W, row0, row1};
        draw_parts(p, &view, &band, true);
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(color + row0 * W, band_color, (row1 - row0) * W,
                                              "a window drawn with cluster rows lost something");
    }
    free(band_depth);
    free(band_color);
}

/* Each triangle reaches over one side of the screen; at 100 away a unit is
 * 0.48 pixels. */
static void
test_a_triangle_over_any_side_of_the_screen_is_drawn(void) {
    static const int16_t over[4][3][3] = {
        {{50, 0, -100}, {100, -20, -100}, {100, 20, -100}},
        {{-50, 0, -100}, {-100, 20, -100}, {-100, -20, -100}},
        {{0, 20, -100}, {20, 80, -100}, {-20, 80, -100}},
        {{0, -20, -100}, {-20, -80, -100}, {20, -80, -100}},
    };
    static const int16_t beyond_right[3][3] = {{80, 0, -100}, {120, -20, -100}, {120, 20, -100}};
    static const uint8_t white[3][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    parts_t* const p = parts_buffer();
    const r3d_lit_view_t view = look_down_minus_z(0, 0, 1.0f);
    for (int side = 0; side < 4; side++) {
        parts_begin(p);
        parts_add(p, over[side], 3, white, true);
        const r3d_span_target_t t = fixture();
        draw_parts(p, &view, &t, false);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, covered(), "a triangle reaching over a side was dropped");
    }
    parts_begin(p);
    parts_add(p, beyond_right, 3, white, true);
    const r3d_span_target_t t = fixture();
    draw_parts(p, &view, &t, false);
    TEST_ASSERT_EQUAL_INT(0, covered());
}

/* Frame */

#define SKY GFX_RGB(0x80C0F0)

/* A wall on the left half and a stack of small rectangles on the right:
 * the rectangles hold most of the triangles, so as the eye rises the row
 * the two cores split at moves down the picture with them. */
static void
build_wall_and_stack(parts_t* p) {
    parts_begin(p);
    static const int16_t wall[4][3] = {{-1000, -1000, -100}, {0, -1000, -100}, {0, 1000, -100}, {-1000, 1000, -100}};
    static const uint8_t wall_rgb[4][3] = {{250, 20, 20}, {20, 250, 20}, {20, 20, 250}, {200, 200, 20}};
    parts_add(p, wall, 4, wall_rgb, false);
    for (int i = 0; i < 8; i++) {
        const int x = 20 + (i % 2) * 90, y = 100 + i * 12;
        parts_add_rect(p, x, y, x + 60, y + 10, 0, (uint8_t)(30 * i));
    }
}

/* One draw of every visible cluster into a full-height target, doubled,
 * with the clear colour wherever nothing was drawn. */
static void
reference_frame(const parts_t* p, const r3d_lit_view_t* view, gfx_color_t* doubled) {
    const r3d_span_target_t full = fixture();
    draw_parts(p, view, &full, false);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const gfx_color_t c = depth[y * W + x] != 0 ? color[y * W + x] : SKY;
            for (int k = 0; k < 4; k++) {
                doubled[(2 * y + k / 2) * 2 * W + 2 * x + k % 2] = c;
            }
        }
    }
}

typedef struct {
    const char* at;
    size_t size;
} span_of_bytes_t;

static void
test_the_frame_carves_its_scratch_without_overlap(void) {
    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    r3d_lit_frame_t frame = {.mesh = &p->mesh, .width = W, .height = H};
    const size_t bytes = r3d_lit_frame_scratch_bytes(&p->mesh, W, H);
    char* scratch = malloc(bytes);
    TEST_ASSERT_NOT_NULL(scratch);
    r3d_lit_frame_use_scratch(&frame, scratch);
    const span_of_bytes_t parts[] = {
        {(const char*)frame.cs, sizeof(r3d_lit_vertex_t) * (size_t)p->mesh.vertex_count},
        {(const char*)frame.rows, sizeof(r3d_lit_rows_t) * (size_t)p->mesh.cluster_count},
        {(const char*)frame.visible, sizeof(uint16_t) * (size_t)p->mesh.cluster_count},
        {(const char*)frame.color, sizeof(uint16_t) * W * H},
        {(const char*)frame.depth, sizeof(uint16_t) * W * H},
    };
    size_t total = 0;
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE_MESSAGE(parts[i].at >= scratch && parts[i].at + parts[i].size <= scratch + bytes,
                                 "a working array reaches outside the scratch");
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_TRUE_MESSAGE(parts[i].at + parts[i].size <= parts[j].at
                                         || parts[j].at + parts[j].size <= parts[i].at,
                                     "two working arrays overlap");
        }
        total += parts[i].size;
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)bytes, (uint32_t)total);
    free(scratch);
}

/* Split between the cores at whatever row balances them, then doubled: the
 * same picture as one full draw doubled, over a colour target left full of
 * stale pixels. */
static void
test_the_two_core_frame_matches_one_full_draw(void) {
    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    gfx_color_t* doubled = malloc(sizeof(gfx_color_t) * 4 * W * H);
    gfx_color_t* want = malloc(sizeof(gfx_color_t) * 4 * W * H);
    char* scratch = malloc(r3d_lit_frame_scratch_bytes(&p->mesh, W, H));
    TEST_ASSERT_NOT_NULL(doubled);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_NOT_NULL(scratch);
    r3d_lit_frame_t frame = {.mesh = &p->mesh, .width = W, .height = H, .clear = SKY, .doubled = doubled};
    r3d_lit_frame_use_scratch(&frame, scratch);

    static const float eye_heights[] = {-100.0f, 0.0f, 150.0f, 230.0f, 300.0f};
    for (int e = 0; e < (int)(sizeof eye_heights / sizeof eye_heights[0]); e++) {
        const r3d_lit_view_t view = look_down_minus_z(eye_heights[e], 400, 1.0f);
        for (int i = 0; i < W * H; i++) {
            frame.color[i] = 0xBEEF;
        }
        r3d_lit_frame_render(&frame, &view);
        r3d_lit_frame_double(&frame);
        reference_frame(p, &view, want);
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(want, doubled, 4 * W * H, "the doubled frame differs from one full draw");
    }

    /* With nothing to double into, the frame clears its own colour target. */
    frame.doubled = NULL;
    const r3d_lit_view_t view = look_down_minus_z(150.0f, 400, 1.0f);
    r3d_lit_frame_render(&frame, &view);
    reference_frame(p, &view, want);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_HEX16(want[2 * y * 2 * W + 2 * x], frame.color[y * W + x]);
        }
    }
    free(scratch);
    free(want);
    free(doubled);
}

/* Camera path */

static const r3d_waypoint_t square[] = {
    {{0, 0, 0}, {0, 0, -1}},
    {{100, 0, 0}, {100, 0, -1}},
    {{100, 0, 100}, {100, 0, 99}},
    {{0, 0, 100}, {0, 0, 99}},
};
static const r3d_path_t square_path = {square, 4, 50.0f, 1.0f};

static void
test_the_path_passes_through_each_waypoint(void) {
    uint32_t t = 0;
    for (int i = 0; i < 4; i++) {
        r3d_vec3f_t eye, forward;
        r3d_path_sample(&square_path, t, &eye, &forward);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, square[i].eye.x, eye.x);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, square[i].eye.z, eye.z);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, forward.z);
        t += 2000; /* 100 units at 50 per second */
    }
    TEST_ASSERT_EQUAL_UINT32(8000, r3d_path_period_ms(&square_path));
}

static void
test_the_path_is_continuous_and_loops(void) {
    const uint32_t period = r3d_path_period_ms(&square_path);
    r3d_vec3f_t prev, forward;
    r3d_path_sample(&square_path, 0, &prev, &forward);
    for (uint32_t t = 10; t <= period; t += 10) {
        r3d_vec3f_t eye;
        r3d_path_sample(&square_path, t, &eye, &forward);
        const float step = sqrtf((eye.x - prev.x) * (eye.x - prev.x) + (eye.z - prev.z) * (eye.z - prev.z));
        TEST_ASSERT_TRUE_MESSAGE(step < 1.5f, "the eye jumped between two samples 10 ms apart");
        prev = eye;
    }
}

/* Two waypoints a unit apart: the eye barely moves, yet each leg still
 * takes the path's minimum time. */
static void
test_a_leg_that_barely_moves_still_takes_the_minimum_time(void) {
    static const r3d_waypoint_t close[] = {{{0, 0, 0}, {0, 0, -1}}, {{1, 0, 0}, {1, 0, -1}}};
    const r3d_path_t path = {close, 2, 50.0f, 1.0f};
    TEST_ASSERT_EQUAL_UINT32(2000, r3d_path_period_ms(&path));
}

/* Every waypoint of the square looks one unit down -z, so the look
 * direction is that between the waypoints too. */
static void
test_forward_is_the_target_minus_the_eye_between_waypoints(void) {
    for (uint32_t t = 0; t < r3d_path_period_ms(&square_path); t += 170) {
        r3d_vec3f_t eye, forward;
        r3d_path_sample(&square_path, t, &eye, &forward);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, forward.x);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, forward.y);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, forward.z);
    }
}

static void
release_fixture(void) {
    free(shared_parts);
    shared_parts = NULL;
    free(depth);
    depth = NULL;
    free(color);
    color = NULL;
}

#undef RUN_TEST
#define RUN_TEST(func)                                                                                                 \
    do {                                                                                                               \
        suite_run_test_timed(func, #func, __LINE__);                                                                   \
        release_fixture();                                                                                             \
    } while (0)

static void
run_r3d_lit_suite(void) {
    RUN_TEST(test_two_triangles_sharing_an_edge_cover_a_square_exactly_once);
    RUN_TEST(test_tiny_and_large_triangles_tile_without_gaps_or_overlap);
    RUN_TEST(test_the_nearer_triangle_wins_in_either_order);
    RUN_TEST(test_a_window_of_rows_matches_the_same_rows_of_a_full_draw);
    RUN_TEST(test_colours_stay_within_the_vertex_range_even_at_the_edges);
    RUN_TEST(test_an_axis_aligned_square_fills_exactly_the_centres_inside_it);
    RUN_TEST(test_a_tiny_triangle_takes_the_average_of_its_corners);
    RUN_TEST(test_a_steep_sliver_puts_no_pixel_nearer_than_its_nearest_corner);
    RUN_TEST(test_every_triangle_covers_exactly_the_centres_the_top_left_rule_gives);
    RUN_TEST(test_a_mesh_of_small_and_large_triangles_fills_every_pixel_exactly_once);
    RUN_TEST(test_a_window_holding_a_few_rows_of_a_tall_sliver_draws_them_as_the_whole_does);
    RUN_TEST(test_a_triangle_with_corners_past_the_snap_range_fills_its_centres);
    RUN_TEST(test_a_triangle_whose_only_centre_is_at_a_window_edge_is_drawn);
    RUN_TEST(test_rebuilt_and_fast_triangles_sharing_edges_fill_every_pixel_once);

    RUN_TEST(test_a_counter_clockwise_face_toward_the_camera_is_drawn);
    RUN_TEST(test_a_face_turned_away_is_culled_unless_double_sided);
    RUN_TEST(test_a_cluster_behind_the_camera_is_culled);
    RUN_TEST(test_a_floor_crossing_the_near_plane_draws_only_below_the_horizon);
    RUN_TEST(test_a_point_up_and_right_lands_up_and_right_in_every_quarter);
    RUN_TEST(test_a_triangle_cut_by_the_near_plane_draws_the_whole_quad_left_in_front);
    RUN_TEST(test_colour_along_the_near_cut_is_interpolated_to_the_cut);
    RUN_TEST(test_a_double_sided_triangle_cut_by_the_near_plane_draws_from_behind);
    RUN_TEST(test_drawing_a_window_with_cluster_rows_matches_a_full_draw);
    RUN_TEST(test_a_triangle_over_any_side_of_the_screen_is_drawn);

    RUN_TEST(test_the_frame_carves_its_scratch_without_overlap);
    RUN_TEST(test_the_two_core_frame_matches_one_full_draw);

    RUN_TEST(test_the_path_passes_through_each_waypoint);
    RUN_TEST(test_the_path_is_continuous_and_loops);
    RUN_TEST(test_a_leg_that_barely_moves_still_takes_the_minimum_time);
    RUN_TEST(test_forward_is_the_target_minus_the_eye_between_waypoints);
}

#undef RUN_TEST

SUITE_REGISTER(run_r3d_lit_suite);
