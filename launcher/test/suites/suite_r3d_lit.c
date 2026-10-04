/*
 * Portable suite: r3d_span.h's triangle fill, r3d_pipeline.h's camera,
 * culling and near clip, raster.h's two-core draw. Every mesh here is built inside the test, never a baked one.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/gfx_color.h"
#include "render/r3d.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"
#include "render/ray.h"
#include "util/memory.h"

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
test_a_flat_triangle_keeps_its_face_colour_and_interpolates_depth(void) {
    const r3d_span_vertex_t a = sv(4, 4, 0.2f, 0, 0, 0), b = sv(52, 4, 0.8f, 0, 0, 0), c = sv(4, 40, 0.5f, 0, 0, 0);
    const uint16_t face = GFX_RGB(0x12AB34);
    r3d_span_target_t t = fixture();
    r3d_span_triangle_solid(&t, &a, &b, &c, face);
    const int near = 8 * W + 8;
    const int far = 8 * W + 40;
    TEST_ASSERT_EQUAL_HEX16(face, color[near]);
    TEST_ASSERT_EQUAL_HEX16(face, color[far]);
    TEST_ASSERT_GREATER_THAN_UINT16(depth[near], depth[far]);
}

static void
test_a_tiny_solid_triangle_takes_its_face_colour(void) {
    const r3d_span_vertex_t a = sv(10.0f, 10.0f, 0.5f, 0, 0, 0), b = sv(12.6f, 10.0f, 0.5f, 0, 0, 0),
                            c = sv(10.0f, 12.6f, 0.5f, 0, 0, 0);
    const uint16_t face = GFX_RGB(0x12AB34);
    r3d_span_target_t t = fixture();
    r3d_span_triangle_solid(&t, &a, &b, &c, face);
    TEST_ASSERT_GREATER_THAN_INT(0, covered());
    for (int i = 0; i < W * H; i++) {
        if (depth[i] != 0) {
            TEST_ASSERT_EQUAL_HEX16(face, color[i]);
        }
    }
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

static void
channels_of(gfx_color_t c, int out[3]) {
    const uint16_t native = (uint16_t)((c >> 8) | (c << 8));
    out[0] = native >> 11;
    out[1] = (native >> 5) & 63;
    out[2] = native & 31;
}

/* Triangles one to five pixels across with a colour and a depth at each
 * corner, the sizes where the one-colour, centre-by-centre and walked paths
 * meet: drawn as two windows they match the whole draw, and no channel
 * leaves its corners' range. */
static void
test_small_shaded_triangles_keep_their_planes_in_range_in_any_window(void) {
    uint32_t state = 4242u;
    int shaded = 0;
    gfx_color_t* whole_color = malloc(sizeof(*whole_color) * W * H);
    uint16_t* whole_depth = malloc(sizeof(*whole_depth) * W * H);
    TEST_ASSERT_NOT_NULL(whole_color);
    TEST_ASSERT_NOT_NULL(whole_depth);
    for (int i = 0; i < 1000; i++) {
        const float size = 0.5f + (2.0f * (float)(next_random(&state) % 1000u) / 1000.0f);
        const float cx = 4.0f + (float)(next_random(&state) % (unsigned)(W - 8));
        const float cy = 4.0f + (float)(next_random(&state) % (unsigned)(H - 8));
        r3d_span_vertex_t v[3];
        int lo[3] = {255, 255, 255};
        int hi[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            const float rgb[3] = {(float)(next_random(&state) % 256u), (float)(next_random(&state) % 256u),
                                  (float)(next_random(&state) % 256u)};
            v[k] = sv(0, 0, 0.1f + (0.8f * (float)(next_random(&state) % 1000u) / 1000.0f), rgb[0], rgb[1], rgb[2]);
            v[k].x = random_subpixel(&state, cx - size, 2.0f * size);
            v[k].y = random_subpixel(&state, cy - size, 2.0f * size);
            for (int ch = 0; ch < 3; ch++) {
                lo[ch] = (int)rgb[ch] < lo[ch] ? (int)rgb[ch] : lo[ch];
                hi[ch] = (int)rgb[ch] > hi[ch] ? (int)rgb[ch] : hi[ch];
            }
        }
        const bool solid = next_random(&state) % 4u == 0;
        const gfx_color_t face = GFX_RGB(0x336699);
        const int split = (int)cy + (int)(next_random(&state) % 5u) - 2;
        r3d_span_target_t t = fixture();
        if (solid) {
            r3d_span_triangle_solid(&t, &v[0], &v[1], &v[2], face);
        } else {
            r3d_span_triangle(&t, &v[0], &v[1], &v[2]);
        }
        memcpy(whole_color, color, sizeof(*color) * W * H);
        memcpy(whole_depth, depth, sizeof(*depth) * W * H);
        t = fixture();
        const r3d_span_target_t top = {t.color, t.depth, W, 0, split};
        const r3d_span_target_t bottom = {t.color + (split * W), t.depth + (split * W), W, split, H};
        if (solid) {
            r3d_span_triangle_solid(&top, &v[0], &v[1], &v[2], face);
            r3d_span_triangle_solid(&bottom, &v[0], &v[1], &v[2], face);
        } else {
            r3d_span_triangle(&top, &v[0], &v[1], &v[2]);
            r3d_span_triangle(&bottom, &v[0], &v[1], &v[2]);
        }
        const r3d_span_extent_t e = r3d_span_extent(&v[0], &v[1], &v[2]);
        shaded += !e.flat && covered() > 0;
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole_depth, depth, sizeof(*depth) * W * H, "a window drew another depth");
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole_color, color, sizeof(*color) * W * H, "a window drew another colour");
        for (int p = 0; p < W * H; p++) {
            if (depth[p] == 0 || solid) {
                TEST_ASSERT_TRUE(depth[p] == 0 || color[p] == face);
                continue;
            }
            int c[3];
            channels_of(color[p], c);
            /* 565 keeps the top bits, and the fixed-point planes may round
             * one step past a corner. */
            TEST_ASSERT_TRUE_MESSAGE(c[0] >= (lo[0] >> 3) - 1 && c[0] <= (hi[0] >> 3) + 1, "red left its corners");
            TEST_ASSERT_TRUE_MESSAGE(c[1] >= (lo[1] >> 2) - 1 && c[1] <= (hi[1] >> 2) + 1, "green left its corners");
            TEST_ASSERT_TRUE_MESSAGE(c[2] >= (lo[2] >> 3) - 1 && c[2] <= (hi[2] >> 3) + 1, "blue left its corners");
        }
    }
    free(whole_color);
    free(whole_depth);
    TEST_ASSERT_GREATER_THAN_INT(100, shaded);
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

static r3d_span_vertex_t
random_vertex(uint32_t* state, float cx, float cy, float radius, float z_lo, float z_span) {
    const float r = (float)(next_random(state) % 256u), g = (float)(next_random(state) % 256u);
    const float b = (float)(next_random(state) % 256u);
    const float z = z_lo + (z_span * (float)(next_random(state) % 1000u) / 1000.0f);
    return (r3d_span_vertex_t){random_subpixel(state, cx - radius, 2.0f * radius),
                               random_subpixel(state, cy - radius, 2.0f * radius),
                               z,
                               r,
                               g,
                               b};
}

/* Anything from half a pixel to thirty across; mostly at depths within a
 * few steps of each other, one in eight steeply sloped, one in eight at the
 * far end of the depth range. */
static void
random_overlapping_triangle(uint32_t* state, r3d_span_vertex_t v[3]) {
    const float radius = 0.5f + (float)(next_random(state) % 300u) / 10.0f;
    const float cx = (float)(next_random(state) % (W + 16u)) - 8.0f;
    const float cy = (float)(next_random(state) % (H + 16u)) - 8.0f;
    const uint32_t kind = next_random(state) % 8u;
    const float z_lo = kind == 0 ? 0.05f : (kind == 1 ? 1e-7f : 0.5f);
    const float z_span = kind == 0 ? 0.9f : (kind == 1 ? 2e-5f : 0.002f);
    for (int k = 0; k < 3; k++) {
        v[k] = random_vertex(state, cx, cy, radius, z_lo, z_span);
    }
}

/* Draws into both halves of the rows, as two windows. */
static void
draw_in_two_windows(gfx_color_t* to_color, uint16_t* to_depth, const r3d_span_vertex_t v[3]) {
    const int split = H / 2 - 3;
    const r3d_span_target_t top = {to_color, to_depth, W, 0, split};
    const r3d_span_target_t bottom = {to_color + (split * W), to_depth + (split * W), W, split, H};
    r3d_span_triangle(&top, &v[0], &v[1], &v[2]);
    r3d_span_triangle(&bottom, &v[0], &v[1], &v[2]);
}

/* Overlapping triangles drawn in turn: each pixel holds what
 * the nearest triangle there would draw alone, so skipping a triangle whose
 * every pixel is already nearer never loses one it would have won. */
static void
test_a_triangle_drawn_over_nearer_depth_writes_exactly_what_it_would_alone(void) {
    uint32_t state = 4242u;
    r3d_span_target_t t = fixture();
    (void)t;
    gfx_color_t* alone_color = malloc(sizeof(*alone_color) * W * H);
    uint16_t* alone_depth = malloc(sizeof(*alone_depth) * W * H);
    gfx_color_t* want_color = calloc(W * H, sizeof(*want_color));
    uint16_t* want_depth = calloc(W * H, sizeof(*want_depth));
    TEST_ASSERT_NOT_NULL(alone_color);
    TEST_ASSERT_NOT_NULL(alone_depth);
    TEST_ASSERT_NOT_NULL(want_color);
    TEST_ASSERT_NOT_NULL(want_depth);
    for (int i = 0; i < 600; i++) {
        r3d_span_vertex_t v[3];
        random_overlapping_triangle(&state, v);
        memset(alone_color, 0, sizeof(*alone_color) * W * H);
        memset(alone_depth, 0, sizeof(*alone_depth) * W * H);
        draw_in_two_windows(alone_color, alone_depth, v);
        for (int p = 0; p < W * H; p++) {
            if (alone_depth[p] > want_depth[p]) {
                want_depth[p] = alone_depth[p];
                want_color[p] = alone_color[p];
            }
        }
        draw_in_two_windows(color, depth, v);
    }
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want_depth, depth, W * H);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(want_color, color, W * H);
    free(alone_color);
    free(alone_depth);
    free(want_color);
    free(want_depth);
}

/* A dropped triangle is one whose bounding box already holds depth at least
 * as near as any it would write. The canvas below is any size, allocated
 * for one test; a split of 0 draws in one window, any other in two windows
 * meeting at that row. */

typedef struct {
    gfx_color_t* color;
    uint16_t* depth;
    uint16_t* alone; /* depth_alone()'s copy */
    int w, h;
} canvas_t;

static canvas_t canvas;

static void
release_canvas(void) {
    memory_free(canvas.color);
    memory_free(canvas.depth);
    memory_free(canvas.alone);
    canvas = (canvas_t){0};
    release_fixture();
}

static canvas_t*
canvas_open(int w, int h) {
    const size_t pixels = (size_t)w * (size_t)h;
    canvas = (canvas_t){memory_alloc(sizeof(gfx_color_t) * pixels, MEMORY_PSRAM),
                        memory_alloc(sizeof(uint16_t) * pixels, MEMORY_PSRAM),
                        memory_alloc(sizeof(uint16_t) * pixels, MEMORY_PSRAM), w, h};
    TEST_ASSERT_NOT_NULL(canvas.color);
    TEST_ASSERT_NOT_NULL(canvas.depth);
    TEST_ASSERT_NOT_NULL(canvas.alone);
    suite_set_test_cleanup(release_canvas);
    return &canvas;
}

static void
canvas_fill(const canvas_t* c, uint16_t z) {
    for (int p = 0; p < c->w * c->h; p++) {
        c->depth[p] = z;
        c->color[p] = 0;
    }
}

static void
canvas_draw(const canvas_t* c, int split, const r3d_span_vertex_t v[3]) {
    const int first_end = split == 0 ? c->h : split;
    const r3d_span_target_t top = {c->color, c->depth, c->w, 0, first_end};
    r3d_span_triangle(&top, &v[0], &v[1], &v[2]);
    if (split != 0) {
        const size_t offset = (size_t)split * (size_t)c->w;
        const r3d_span_target_t bottom = {c->color + offset, c->depth + offset, c->w, split, c->h};
        r3d_span_triangle(&bottom, &v[0], &v[1], &v[2]);
    }
}

/* Draws v on an empty canvas and returns a copy of the depth it left. */
static const uint16_t*
depth_alone(const canvas_t* c, int split, const r3d_span_vertex_t v[3]) {
    canvas_fill(c, R3D_DEPTH_EMPTY);
    canvas_draw(c, split, v);
    memcpy(c->alone, c->depth, sizeof(uint16_t) * (size_t)(c->w * c->h));
    return c->alone;
}

/* Far outside the screen on both sides, its depth rising steeply across
 * it: the plane at its far box corners is far past any 32-bit sum. */
static void
test_a_guard_band_sliver_draws_its_centres_in_one_window_or_two(void) {
    const canvas_t* c = canvas_open(GFX_WIDTH, GFX_HEIGHT);
    const r3d_span_vertex_t v[3] = {sv(-500.0f, 501.0f, 0.5f, 200, 100, 50), sv(501.0f, -500.0f, 0.5f, 200, 100, 50),
                                    sv(-499.0f, 503.0f, 1.0f, 200, 100, 50)};
    const int splits[2] = {0, c->h / 2};
    for (int s = 0; s < 2; s++) {
        canvas_fill(c, R3D_DEPTH_EMPTY);
        canvas_draw(c, splits[s], v);
        int inside = 0;
        for (int y = 0; y < c->h; y++) {
            for (int x = 0; x < c->w; x++) {
                const bool want = reference_inside(&v[0], &v[1], &v[2], x, y);
                inside += want;
                TEST_ASSERT_EQUAL_MESSAGE(want, c->depth[(y * c->w) + x] != R3D_DEPTH_EMPTY,
                                          "the sliver's centres differ from the rule");
            }
        }
        TEST_ASSERT_GREATER_THAN_INT(0, inside);
    }
}

/* Over depth one step below the greatest the triangle writes, exactly its
 * pixels at that depth are drawn; at it or above, nothing changes. A bound
 * too cautious by any amount passes too: a tie keeps the pixel already
 * there, so it cannot be seen. */
static void
assert_the_bound_is_exact(const canvas_t* c, int split, const r3d_span_vertex_t v[3]) {
    const uint16_t* alone = depth_alone(c, split, v);
    uint16_t most = 0;
    for (int p = 0; p < c->w * c->h; p++) {
        most = alone[p] > most ? alone[p] : most;
    }
    TEST_ASSERT_GREATER_THAN_UINT16(1, most);
    for (int step = -1; step <= 1; step++) {
        const uint16_t under = (uint16_t)(most + step);
        canvas_fill(c, under);
        canvas_draw(c, split, v);
        for (int p = 0; p < c->w * c->h; p++) {
            const uint16_t want = step < 0 && alone[p] == most ? most : under;
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(want, c->depth[p], "a pixel at the greatest depth was lost or changed");
        }
    }
}

static void
test_the_bound_is_the_greatest_depth_whichever_way_the_plane_slopes(void) {
    const canvas_t* c = canvas_open(W, H);
    const r3d_span_vertex_t nearer_down[3] = {sv(8.3f, 4.2f, 0.3f, 9, 9, 9), sv(56.6f, 4.2f, 0.3f, 9, 9, 9),
                                              sv(31.7f, 44.1f, 0.8f, 9, 9, 9)};
    const r3d_span_vertex_t nearer_up[3] = {sv(8.3f, 44.1f, 0.3f, 9, 9, 9), sv(56.6f, 44.1f, 0.3f, 9, 9, 9),
                                            sv(31.7f, 4.2f, 0.8f, 9, 9, 9)};
    const r3d_span_vertex_t nearer_right[3] = {sv(4.2f, 6.3f, 0.3f, 9, 9, 9), sv(4.2f, 42.6f, 0.3f, 9, 9, 9),
                                               sv(60.1f, 24.4f, 0.8f, 9, 9, 9)};
    const r3d_span_vertex_t nearer_left[3] = {sv(60.1f, 6.3f, 0.3f, 9, 9, 9), sv(60.1f, 42.6f, 0.3f, 9, 9, 9),
                                              sv(4.2f, 24.4f, 0.8f, 9, 9, 9)};
    /* Three centres across and two down: one depth for all. */
    const r3d_span_vertex_t flat[3] = {sv(10.2f, 10.2f, 0.3f, 9, 9, 9), sv(13.1f, 10.4f, 0.5f, 9, 9, 9),
                                       sv(11.0f, 12.1f, 0.8f, 9, 9, 9)};
    const r3d_span_vertex_t* const all[] = {nearer_down, nearer_up, nearer_right, nearer_left, flat};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        assert_the_bound_is_exact(c, 0, all[i]);
        assert_the_bound_is_exact(c, c->h / 2, all[i]);
    }
}

/* A sliver whose plane, stepped at the clamped slope, goes below zero at
 * a span's first centre: the clamped start lifts the whole span. */
static void
test_a_span_start_clamped_up_from_below_zero_still_bounds_the_span(void) {
    const canvas_t* c = canvas_open(W, H);
    const r3d_span_vertex_t v[3] = {sv(57.3446f, 10.1488f, 0.005989f, 9, 9, 9),
                                    sv(-39.1719f, 20.6818f, 0.001197f, 9, 9, 9),
                                    sv(57.3209f, 10.2917f, 0.851835f, 9, 9, 9)};
    assert_the_bound_is_exact(c, 0, v);
    assert_the_bound_is_exact(c, c->h / 2, v);
}

/* Every pixel at the nearest depth but one the triangle covers: that one
 * pixel is enough to draw the triangle. */
static void
assert_one_open_pixel_is_drawn(const canvas_t* c, int split, const r3d_span_vertex_t v[3], int p) {
    canvas_fill(c, R3D_DEPTH_NEAREST);
    c->depth[p] = R3D_DEPTH_EMPTY;
    canvas_draw(c, split, v);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(R3D_DEPTH_EMPTY, c->depth[p], "the one pixel left open was not drawn");
}

/* The covered pixels farthest in each direction: the box's own edges. */
static void
assert_each_extreme_pixel_is_found(const canvas_t* c, int split, const r3d_span_vertex_t v[3]) {
    const uint16_t* alone = depth_alone(c, split, v);
    int extreme[4] = {-1, -1, -1, -1}; /* leftmost, rightmost, topmost, bottommost */
    for (int p = 0; p < c->w * c->h; p++) {
        if (alone[p] == R3D_DEPTH_EMPTY) {
            continue;
        }
        const int x = p % c->w;
        extreme[0] = extreme[0] < 0 || x < extreme[0] % c->w ? p : extreme[0];
        extreme[1] = extreme[1] < 0 || x > extreme[1] % c->w ? p : extreme[1];
        extreme[2] = extreme[2] < 0 ? p : extreme[2];
        extreme[3] = p;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, extreme[0]);
    for (int e = 0; e < 4; e++) {
        assert_one_open_pixel_is_drawn(c, split, v, extreme[e]);
    }
}

static void
test_one_open_pixel_anywhere_in_the_box_draws_the_triangle(void) {
    const canvas_t* c = canvas_open(W, H);
    /* Edges on pixel edges, so the box's last row and column are covered. */
    const r3d_span_vertex_t upper[3] = {sv(4.0f, 4.0f, 0.5f, 9, 9, 9), sv(40.0f, 4.0f, 0.5f, 9, 9, 9),
                                        sv(40.0f, 30.0f, 0.6f, 9, 9, 9)};
    const r3d_span_vertex_t lower[3] = {sv(4.0f, 4.0f, 0.5f, 9, 9, 9), sv(4.0f, 30.0f, 0.5f, 9, 9, 9),
                                        sv(40.0f, 30.0f, 0.6f, 9, 9, 9)};
    const r3d_span_vertex_t past_both_sides[3] = {sv(-20.0f, 5.0f, 0.5f, 9, 9, 9),
                                                  sv((float)W + 20.0f, 8.0f, 0.6f, 9, 9, 9),
                                                  sv(10.0f, (float)H + 10.0f, 0.4f, 9, 9, 9)};
    const r3d_span_vertex_t guard_band[3] = {sv(-900.0f, -800.0f, 0.5f, 9, 9, 9), sv(900.0f, 10.0f, 0.6f, 9, 9, 9),
                                             sv(-800.0f, 900.0f, 0.4f, 9, 9, 9)};
    const r3d_span_vertex_t* const all[] = {upper, lower, past_both_sides, guard_band};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        assert_each_extreme_pixel_is_found(c, 0, all[i]);
        assert_each_extreme_pixel_is_found(c, c->h / 2, all[i]);
    }
}

/* One window's rows all hidden say nothing about the other window's. */
static void
test_a_triangle_hidden_in_one_window_still_draws_in_the_other(void) {
    const canvas_t* c = canvas_open(W, H);
    const int split = c->h / 2;
    const r3d_span_vertex_t v[3] = {sv(6.3f, 3.1f, 0.5f, 9, 9, 9), sv(58.2f, 9.7f, 0.6f, 9, 9, 9),
                                    sv(20.4f, 44.9f, 0.4f, 9, 9, 9)};
    const uint16_t* alone = depth_alone(c, split, v);
    for (int hidden_top = 0; hidden_top < 2; hidden_top++) {
        canvas_fill(c, R3D_DEPTH_EMPTY);
        const int from = hidden_top ? 0 : split * c->w;
        const int to = hidden_top ? split * c->w : c->w * c->h;
        for (int p = from; p < to; p++) {
            c->depth[p] = R3D_DEPTH_NEAREST;
        }
        canvas_draw(c, split, v);
        for (int p = 0; p < c->w * c->h; p++) {
            const uint16_t want = p >= from && p < to ? R3D_DEPTH_NEAREST : alone[p];
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(want, c->depth[p], "the open window was not drawn as alone");
        }
    }
}

static void
draw_wall(const canvas_t* c, float z) {
    const r3d_span_vertex_t a[3] = {sv(0.0f, 0.0f, z, 9, 9, 9), sv((float)c->w, 0.0f, z, 9, 9, 9),
                                    sv((float)c->w, (float)c->h, z, 9, 9, 9)};
    const r3d_span_vertex_t b[3] = {sv(0.0f, 0.0f, z, 9, 9, 9), sv((float)c->w, (float)c->h, z, 9, 9, 9),
                                    sv(0.0f, (float)c->h, z, 9, 9, 9)};
    canvas_draw(c, 0, a);
    canvas_draw(c, 0, b);
}

/* The cheap exit has to be taken, not only be safe: a plane whose nearest
 * corner is behind a wall is reported hidden, and one reaching past the
 * wall at its far corner is not. Planes are 16.8, one step a column. */
static void
test_a_plane_behind_a_wall_is_hidden_and_one_reaching_past_it_is_not(void) {
    const canvas_t* c = canvas_open(W, H);
    canvas_fill(c, R3D_DEPTH_EMPTY);
    draw_wall(c, 0.9f);
    const int32_t wall = c->depth[0];
    const r3d_span_target_t t = {c->color, c->depth, c->w, 0, c->h};
    const r3d_span_box_t box = {5, 15, 3, 9};
    const int32_t step = 1 << 8;
    TEST_ASSERT_EQUAL_INT32(wall - 1, r3d_span_plane_bound((wall - 10) * step, step, 0, box));
    TEST_ASSERT_TRUE(r3d_span_hidden(&t, r3d_span_plane_bound((wall - 10) * step, step, 0, box), box));
    TEST_ASSERT_TRUE(r3d_span_hidden(&t, r3d_span_plane_bound((wall - 5) * step, 0, step, box), box));
    TEST_ASSERT_TRUE(r3d_span_hidden(&t, wall, box));
    TEST_ASSERT_EQUAL_INT32(wall + 4, r3d_span_plane_bound((wall - 5) * step, step, 0, box));
    TEST_ASSERT_FALSE(r3d_span_hidden(&t, r3d_span_plane_bound((wall - 5) * step, step, 0, box), box));
    TEST_ASSERT_FALSE(r3d_span_hidden(&t, wall + 1, box));
    /* One pixel of the box farther than the bound is enough. */
    c->depth[(box.y1 - 1) * c->w + (box.x1 - 1)] = (uint16_t)(wall - 2);
    TEST_ASSERT_FALSE(r3d_span_hidden(&t, wall - 1, box));
}

/* A start clamped up from below zero lifts its span by the shortfall. */
static void
test_a_plane_below_zero_at_a_corner_is_bounded_by_its_lift(void) {
    const r3d_span_box_t box = {0, 11, 0, 1};
    const int32_t step = 1 << 8;
    TEST_ASSERT_EQUAL_INT32(10, r3d_span_plane_bound(-3 * step, step, 0, box));
    TEST_ASSERT_EQUAL_INT32(R3D_DEPTH_NEAREST, r3d_span_plane_bound(INT32_MAX, INT32_MAX, INT32_MAX, box));
}

/* A span skips its clamps only when the plane is inside the range at every
 * centre of the box: checked against each centre, with planes small enough
 * that a corner often lands on 0, on the range's top, or one past either. */
static void
test_a_plane_is_in_range_exactly_when_every_centre_of_its_box_is(void) {
    uint32_t state = 0x1a2b3c4du;
    int inside = 0;
    int outside = 0;
    for (int i = 0; i < 20000; i++) {
        const int x0 = (int)(next_random(&state) % 20u), y0 = (int)(next_random(&state) % 20u);
        const r3d_span_box_t box = {x0, x0 + 1 + (int)(next_random(&state) % 12u), y0,
                                    y0 + 1 + (int)(next_random(&state) % 12u)};
        const int32_t top = (int32_t)(next_random(&state) % 320u) - 32;
        const int32_t dx = (int32_t)(next_random(&state) % 41u) - 20;
        const int32_t dy = (int32_t)(next_random(&state) % 41u) - 20;
        bool every = true;
        for (int y = box.y0; y < box.y1; y++) {
            for (int x = box.x0; x < box.x1; x++) {
                const int32_t v = top + (dx * (x - box.x0)) + (dy * (y - box.y0));
                every = every && v >= 0 && v <= 255;
            }
        }
        inside += every;
        outside += !every;
        TEST_ASSERT_EQUAL_MESSAGE(every, r3d_span_plane_in_range(top, dx, dy, 255, box),
                                  "the range check disagrees with the box's centres");
    }
    TEST_ASSERT_GREATER_THAN_INT(1000, inside);
    TEST_ASSERT_GREATER_THAN_INT(1000, outside);
    /* Past any 32-bit sum at the far corner. */
    const r3d_span_box_t wide = {0, 1000, 0, 1000};
    TEST_ASSERT_FALSE(r3d_span_plane_in_range(0, INT32_MAX / 2, INT32_MAX / 2, INT32_MAX, wide));
}

/* Behind a nearer wall, a triangle leaves both buffers as they were. */
static void
test_triangles_behind_a_nearer_wall_leave_colour_and_depth_untouched(void) {
    const canvas_t* c = canvas_open(W, H);
    canvas_fill(c, R3D_DEPTH_EMPTY);
    draw_wall(c, 0.9f);
    memcpy(c->alone, c->depth, sizeof(uint16_t) * (size_t)(c->w * c->h));
    gfx_color_t* colour = malloc(sizeof(gfx_color_t) * (size_t)(c->w * c->h));
    TEST_ASSERT_NOT_NULL(colour);
    memcpy(colour, c->color, sizeof(gfx_color_t) * (size_t)(c->w * c->h));
    for (int i = 0; i < 10; i++) {
        const float x = 2.3f + (4.9f * (float)i);
        const r3d_span_vertex_t v[3] = {sv(x, 3.1f, 0.2f, 99, 9, 9), sv(x + 11.2f, 20.6f, 0.4f, 9, 99, 9),
                                        sv(x + 1.7f, 41.3f, 0.3f, 9, 9, 99)};
        canvas_draw(c, c->h / 2, v);
    }
    const bool same = memcmp(colour, c->color, sizeof(gfx_color_t) * (size_t)(c->w * c->h)) == 0;
    free(colour);
    TEST_ASSERT_TRUE_MESSAGE(same, "a triangle behind the wall changed the colour");
    TEST_ASSERT_EQUAL_HEX16_ARRAY(c->alone, c->depth, c->w * c->h);
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

static r3d_lens_t
subpixel_view(void) {
    const float s = 1.0f / (float)R3D_SUBPIXEL;
    return (r3d_lens_t){{{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, 1, 0}},
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
    return (r3d_lit_mesh_t){.positions = m->positions,
                            .colors = m->colors,
                            .triangles = m->triangles,
                            .clusters = &m->cluster,
                            .nodes = &m->node,
                            .vertex_count = vertices,
                            .triangle_count = triangles,
                            .cluster_count = 1,
                            .node_count = 1,
                            .position_scale = 1};
}

/* Transforms the mesh with cluster rows and draws it as two windows. */
static void
draw_screen_mesh(const r3d_lit_mesh_t* mesh, int split) {
    const r3d_lens_t lens = subpixel_view();
    const uint16_t visible[1] = {0};
    r3d_pipeline_vertex_t cs[9];
    r3d_pipeline_rows_t rows[1];
    r3d_pipeline_transform(mesh, &lens, visible, 1, cs, rows);
    r3d_span_target_t t = fixture();
    const r3d_span_target_t top = {t.color, t.depth, W, 0, split};
    const r3d_span_target_t bottom = {t.color + (split * W), t.depth + (split * W), W, split, H};
    r3d_pipeline_draw(mesh, &lens, visible, 1, cs, rows, &top);
    r3d_pipeline_draw(mesh, &lens, visible, 1, cs, rows, &bottom);
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
 * r3d_pipeline_draw: the cull that drops triangles holding no centre keeps it. */
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
        const r3d_lens_t lens = subpixel_view();
        const uint16_t visible[1] = {0};
        r3d_pipeline_vertex_t cs[3];
        r3d_pipeline_rows_t rows[1];
        r3d_pipeline_transform(&mesh, &lens, visible, 1, cs, rows);
        r3d_span_target_t t = fixture();
        const r3d_span_target_t window = {t.color + (row0 * W), t.depth + (row0 * W), W, row0, row1};
        r3d_pipeline_draw(&mesh, &lens, visible, 1, cs, rows, &window);
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

/* A mesh lit by its vertices, or by `face_colors` alone when given. */
static r3d_lit_mesh_t
quad_mesh(const uint16_t (*triangles)[3], bool double_sided, const uint16_t* face_colors, r3d_lit_cluster_t* cluster) {
    *cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {-100, -100, 0}, {100, 100, 0}, double_sided};
    return (r3d_lit_mesh_t){.positions = quad_positions,
                            .colors = face_colors == NULL ? quad_colors : NULL,
                            .face_colors = face_colors,
                            .triangles = triangles,
                            .clusters = cluster,
                            .nodes = &quad_node,
                            .vertex_count = 4,
                            .triangle_count = 2,
                            .cluster_count = 1,
                            .node_count = 1,
                            .position_scale = 1};
}

/* The quad faces +z; this camera stands on +z looking back at it. */
static int
draw_quad_with(const uint16_t (*triangles)[3], bool double_sided, const uint16_t* face_colors) {
    r3d_lit_cluster_t cluster;
    const r3d_lit_mesh_t mesh = quad_mesh(triangles, double_sided, face_colors, &cluster);
    r3d_lens_t lens;
    r3d_lens_init(&lens, &(camera_t){{0, 0, 400}, {0, 0, -1}, 0.5f, 1.0f}, 1, (viewport_t){W, H, 0});
    uint16_t visible[1];
    const int count = r3d_pipeline_cull(&mesh, &lens, visible);
    r3d_pipeline_vertex_t cs[4];
    r3d_pipeline_transform(&mesh, &lens, visible, count, cs, NULL);
    const r3d_span_target_t t = fixture();
    r3d_pipeline_draw(&mesh, &lens, visible, count, cs, NULL, &t);
    return covered();
}

static int
draw_quad(const uint16_t (*triangles)[3], bool double_sided) {
    return draw_quad_with(triangles, double_sided, NULL);
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
    const r3d_lit_mesh_t mesh = quad_mesh(quad_front, false, NULL, &cluster);
    r3d_lens_t lens;
    r3d_lens_init(&lens, &(camera_t){{0, 0, 400}, {0, 0, 1}, 0.5f, 1.0f}, 1, (viewport_t){W, H, 0});
    uint16_t visible[1];
    TEST_ASSERT_EQUAL_INT(0, r3d_pipeline_cull(&mesh, &lens, visible));
}

/* A floor running from behind the camera to far ahead, seen from above it
 * with its near half behind the near plane. */
static void
draw_floor(const uint16_t* face_colors) {
    static const int16_t floor_positions[][3] = {
        {-1000, 0, 1000}, {1000, 0, 1000}, {1000, 0, -3000}, {-1000, 0, -3000}};
    static const uint16_t floor_up[][3] = {{0, 1, 2}, {0, 2, 3}};
    r3d_lit_cluster_t cluster = {0, 4, 0, 2, {-1000, 0, -3000}, {1000, 0, 1000}, false};
    static const r3d_lit_node_t floor_node = {{-1000, 0, -3000}, {1000, 0, 1000}, 0, 1, true};
    const r3d_lit_mesh_t mesh = {.positions = floor_positions,
                                 .colors = face_colors == NULL ? quad_colors : NULL,
                                 .face_colors = face_colors,
                                 .triangles = floor_up,
                                 .clusters = &cluster,
                                 .nodes = &floor_node,
                                 .vertex_count = 4,
                                 .triangle_count = 2,
                                 .cluster_count = 1,
                                 .node_count = 1,
                                 .position_scale = 1};

    r3d_lens_t lens;
    r3d_lens_init(&lens, &(camera_t){{0, 50, 0}, {0, 0, -1}, 0.5f, 1.0f}, 1, (viewport_t){W, H, 0});
    uint16_t visible[1];
    const int count = r3d_pipeline_cull(&mesh, &lens, visible);
    TEST_ASSERT_EQUAL_INT(1, count);
    r3d_pipeline_vertex_t cs[4];
    r3d_pipeline_transform(&mesh, &lens, visible, count, cs, NULL);
    const r3d_span_target_t t = fixture();
    r3d_pipeline_draw(&mesh, &lens, visible, count, cs, NULL, &t);
}

/* The near clip keeps the part in front and nothing lands above the horizon row. */
static void
test_a_floor_crossing_the_near_plane_draws_only_below_the_horizon(void) {
    draw_floor(NULL);
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

static void
expect_only_these_two_colours(uint16_t first, uint16_t second, int* first_count, int* second_count) {
    *first_count = *second_count = 0;
    for (int i = 0; i < W * H; i++) {
        if (depth[i] == 0) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(color[i] == first || color[i] == second, "a pixel took a colour no face carries");
        *first_count += color[i] == first;
        *second_count += color[i] == second;
    }
}

static void
test_a_mesh_with_face_colours_draws_each_triangle_in_its_own(void) {
    static const uint16_t faces[2] = {GFX_RGB(0x2040E0), GFX_RGB(0xE0A020)};
    TEST_ASSERT_EQUAL_INT(24 * 24, draw_quad_with(quad_front, false, faces));
    int first, second;
    expect_only_these_two_colours(faces[0], faces[1], &first, &second);
    TEST_ASSERT_GREATER_THAN_INT(0, first);
    TEST_ASSERT_GREATER_THAN_INT(0, second);
    TEST_ASSERT_EQUAL_INT(24 * 24, first + second);
}

static void
test_face_colours_survive_a_triangle_clipped_by_the_near_plane(void) {
    static const uint16_t faces[2] = {GFX_RGB(0x2040E0), GFX_RGB(0xE0A020)};
    draw_floor(faces);
    int first, second;
    expect_only_these_two_colours(faces[0], faces[1], &first, &second);
    TEST_ASSERT_GREATER_THAN_INT(0, first);
    TEST_ASSERT_GREATER_THAN_INT(0, second);
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
        const viewport_t viewport = {W, H, quarter};
        r3d_lens_t lens;
        r3d_lens_init(&lens, &(camera_t){{0, 0, 0}, {0, 0, -1}, 0.5f, 1.0f}, 1, viewport);
        const float px = lens.m[0][0] * 30 + lens.m[0][1] * 20 + lens.m[0][2] * -100 + lens.m[0][3];
        const float py = lens.m[1][0] * 30 + lens.m[1][1] * 20 + lens.m[1][2] * -100 + lens.m[1][3];
        const float pz = lens.m[2][0] * 30 + lens.m[2][1] * 20 + lens.m[2][2] * -100 + lens.m[2][3];
        int ux, uy;
        viewport_physical_to_upright(viewport, (int)floorf(lens.center_x + px / pz),
                                     (int)floorf(lens.center_y + py / pz), &ux, &uy);

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
    p->mesh = (r3d_lit_mesh_t){.positions = p->positions,
                               .colors = p->colors,
                               .triangles = p->triangles,
                               .clusters = p->clusters,
                               .nodes = &p->node,
                               .node_count = 1,
                               .position_scale = 1};
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

static camera_t
camera_down_minus_z(float eye_y, float eye_z, float near_z) {
    return (camera_t){{0, eye_y, eye_z}, {0, 0, -1}, 0.5f, near_z};
}

/* The lens raster_draw() makes of camera_down_minus_z() for a W by H
 * raster of a mesh at position scale 1. */
static r3d_lens_t
look_down_minus_z(float eye_y, float eye_z, float near_z) {
    const camera_t camera = camera_down_minus_z(eye_y, eye_z, near_z);
    r3d_lens_t lens;
    r3d_lens_init(&lens, &camera, 1, (viewport_t){W, H, 0});
    return lens;
}

/* Cull, transform and draw every cluster into `t`; with `use_rows`, the
 * draw skips clusters by their rows. */
static void
draw_parts(const parts_t* p, const r3d_lens_t* lens, const r3d_span_target_t* t, bool use_rows) {
    uint16_t* visible = malloc(sizeof(*visible) * PARTS_MAX);
    r3d_pipeline_vertex_t* cs = malloc(sizeof(*cs) * PARTS_MAX * 4);
    r3d_pipeline_rows_t* rows = malloc(sizeof(*rows) * PARTS_MAX);
    TEST_ASSERT_NOT_NULL(visible);
    TEST_ASSERT_NOT_NULL(cs);
    TEST_ASSERT_NOT_NULL(rows);
    const int count = r3d_pipeline_cull(&p->mesh, lens, visible);
    r3d_pipeline_transform(&p->mesh, lens, visible, count, cs, use_rows ? rows : NULL);
    r3d_pipeline_draw(&p->mesh, lens, visible, count, cs, use_rows ? rows : NULL, t);
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
    const r3d_lens_t lens = look_down_minus_z(0, 0, 100.0f);
    const r3d_span_target_t t = fixture();
    draw_parts(p, &lens, &t, false);
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
    const r3d_lens_t lens = look_down_minus_z(50, 0, 1.0f);
    const r3d_span_target_t full = fixture();
    draw_parts(p, &lens, &full, false);

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
        draw_parts(p, &lens, &band, true);
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
    const r3d_lens_t lens = look_down_minus_z(0, 0, 1.0f);
    for (int side = 0; side < 4; side++) {
        parts_begin(p);
        parts_add(p, over[side], 3, white, true);
        const r3d_span_target_t t = fixture();
        draw_parts(p, &lens, &t, false);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, covered(), "a triangle reaching over a side was dropped");
    }
    parts_begin(p);
    parts_add(p, beyond_right, 3, white, true);
    const r3d_span_target_t t = fixture();
    draw_parts(p, &lens, &t, false);
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

/* One draw of every visible cluster into a full-height target, upscaled,
 * with the clear colour wherever nothing was drawn. */
static void
reference_frame(const parts_t* p, const r3d_lens_t* lens, gfx_color_t* upscaled) {
    const r3d_span_target_t full = fixture();
    draw_parts(p, lens, &full, false);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const gfx_color_t c = depth[y * W + x] != 0 ? color[y * W + x] : SKY;
            for (int k = 0; k < 4; k++) {
                upscaled[(2 * y + k / 2) * 2 * W + 2 * x + k % 2] = c;
            }
        }
    }
}

/* A raster's one mesh, as the instance it is. */
#define ONE_MESH(m) .instances = &(const r3d_instance_t){(m), NULL}, .instance_count = 1

typedef struct {
    const char* at;
    size_t size;
} span_of_bytes_t;

static void
test_the_frame_carves_its_scratch_without_overlap(void) {
    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    raster_t raster = {ONE_MESH(&p->mesh), .width = W, .height = H};
    const size_t bytes = raster_scratch_bytes(&raster);
    char* scratch = malloc(bytes);
    TEST_ASSERT_NOT_NULL(scratch);
    raster.scratch = scratch;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(&raster);
    const span_of_bytes_t parts[] = {
        {(const char*)b.cs, sizeof(r3d_pipeline_vertex_t) * (size_t)p->mesh.vertex_count},
        {(const char*)b.rows, sizeof(r3d_pipeline_rows_t) * (size_t)p->mesh.cluster_count},
        {(const char*)b.visible, sizeof(uint16_t) * (size_t)p->mesh.cluster_count},
        {(const char*)b.color, sizeof(uint16_t) * W * H},
        {(const char*)b.depth, sizeof(uint16_t) * W * H},
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

/* Split between the cores at whatever row balances them, then upscaled: the
 * same picture as one full draw upscaled, over a colour target left full of
 * stale pixels. */
static void
test_the_two_core_frame_matches_one_full_draw(void) {
    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    gfx_color_t* upscaled = malloc(sizeof(gfx_color_t) * 4 * W * H);
    gfx_color_t* want = malloc(sizeof(gfx_color_t) * 4 * W * H);
    char* scratch = malloc(raster_scratch_bytes(&(raster_t){ONE_MESH(&p->mesh), .width = W, .height = H,
                                                            .destination_width = 2 * W, .destination_height = 2 * H}));
    TEST_ASSERT_NOT_NULL(upscaled);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_NOT_NULL(scratch);
    raster_t raster = {ONE_MESH(&p->mesh),
                       .width = W,
                       .height = H,
                       .clear = SKY,
                       .upscaled = true,
                       .destination = upscaled,
                       .destination_width = 2 * W,
                       .destination_height = 2 * H};
    raster.scratch = scratch;

    static const float eye_heights[] = {-100.0f, 0.0f, 150.0f, 230.0f, 300.0f};
    for (int e = 0; e < (int)(sizeof eye_heights / sizeof eye_heights[0]); e++) {
        const camera_t camera = camera_down_minus_z(eye_heights[e], 400, 1.0f);
        const r3d_lens_t lens = look_down_minus_z(eye_heights[e], 400, 1.0f);
        for (int i = 0; i < W * H; i++) {
            r3d_pipeline_carve(&raster).color[i] = 0xBEEF;
        }
        raster_draw(&raster, &camera, 0);
        raster_upscale(&raster);
        reference_frame(p, &lens, want);
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(want, upscaled, 4 * W * H,
                                              "the upscaled frame differs from one full draw");
    }

    /* With nothing to upscale into, the raster clears its own colour target. */
    raster.upscaled = false;
    const camera_t camera = camera_down_minus_z(150.0f, 400, 1.0f);
    const r3d_lens_t lens = look_down_minus_z(150.0f, 400, 1.0f);
    raster_draw(&raster, &camera, 0);
    reference_frame(p, &lens, want);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_HEX16(want[2 * y * 2 * W + 2 * x], r3d_pipeline_carve(&raster).color[y * W + x]);
        }
    }
    free(scratch);
    free(want);
    free(upscaled);
}

/* A destination the raster's own size is a copy of it, taking
 * the clear colour wherever nothing was drawn. */
static void
test_a_destination_of_the_same_size_is_a_copy(void) {
    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    gfx_color_t* destination = malloc(sizeof(gfx_color_t) * W * H);
    gfx_color_t* want = malloc(sizeof(gfx_color_t) * 4 * W * H);
    char* scratch = malloc(raster_scratch_bytes(
        &(raster_t){ONE_MESH(&p->mesh), .width = W, .height = H, .destination_width = W, .destination_height = H}));
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_NOT_NULL(scratch);
    raster_t raster = {ONE_MESH(&p->mesh),
                       .width = W,
                       .height = H,
                       .clear = SKY,
                       .upscaled = true,
                       .destination = destination,
                       .destination_width = W,
                       .destination_height = H};
    raster.scratch = scratch;

    const camera_t camera = camera_down_minus_z(150.0f, 400, 1.0f);
    const r3d_lens_t lens = look_down_minus_z(150.0f, 400, 1.0f);
    raster_draw(&raster, &camera, 0);
    raster_upscale(&raster);
    reference_frame(p, &lens, want);
    int sky = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_HEX16(want[2 * y * 2 * W + 2 * x], destination[y * W + x]);
            sky += destination[y * W + x] == SKY;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(sky > 0 && sky < W * H, "the view shows both the parts and the clear colour");
    free(scratch);
    free(want);
    free(destination);
}

static int
nearest_source_index(int destination, int destination_size, int source_size) {
    const int numerator = destination * (source_size - 1);
    return (numerator + (destination_size - 1) / 2) / (destination_size - 1);
}

static void
test_a_fractional_destination_upscales_a_drawn_frame(void) {
    enum { OUT_W = 3 * W / 2, OUT_H = 3 * H / 2 };

    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    uint16_t* destination = malloc(sizeof(*destination) * OUT_W * OUT_H);
    uint16_t* color = malloc(sizeof(*color) * W * H);
    uint16_t* depth = malloc(sizeof(*depth) * W * H);
    raster_t raster = {ONE_MESH(&p->mesh),
                       .width = W,
                       .height = H,
                       .clear = SKY,
                       .upscaled = true,
                       .destination = destination,
                       .destination_width = OUT_W,
                       .destination_height = OUT_H};
    raster.scratch = malloc(raster_scratch_bytes(&raster));
    TEST_ASSERT_NOT_NULL(destination);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    TEST_ASSERT_NOT_NULL(raster.scratch);

    const camera_t camera = camera_down_minus_z(150.0f, 400, 1.0f);
    raster_draw(&raster, &camera, 0);
    memcpy(color, r3d_pipeline_carve(&raster).color, sizeof(*color) * W * H);
    memcpy(depth, r3d_pipeline_carve(&raster).depth, sizeof(*depth) * W * H);
    raster_upscale(&raster);
    for (int y = 0; y < OUT_H; y++) {
        for (int x = 0; x < OUT_W; x++) {
            const int source_x = nearest_source_index(x, OUT_W, W);
            const int source_y = nearest_source_index(y, OUT_H, H);
            const int source = source_y * W + source_x;
            TEST_ASSERT_EQUAL_HEX16(depth[source] == R3D_DEPTH_EMPTY ? SKY : color[source], destination[y * OUT_W + x]);
        }
    }
    free(raster.scratch);
    free(depth);
    free(color);
    free(destination);
}

/* Views of the depth */

typedef struct {
    r3d_instance_t instance;
    raster_t raster;
    r3d_lit_mesh_t mesh; /* nothing to draw: its scratch is colour and depth */
} shown_t;

static shown_t* shown;

static void
release_shown(void) {
    if (shown != NULL) {
        free(shown->raster.scratch);
        free(shown);
        shown = NULL;
    }
}

/* A raster of `width` by `height` with nothing drawn and a clear colour that
 * is no grey. */
static raster_t*
shown_frame(int width, int height) {
    release_shown();
    shown = calloc(1, sizeof *shown);
    TEST_ASSERT_NOT_NULL(shown);
    suite_set_test_cleanup(release_shown);
    const size_t count = (size_t)width * (size_t)height;
    shown->raster.width = width;
    shown->raster.height = height;
    shown->raster.clear = SKY;
    shown->instance = (r3d_instance_t){&shown->mesh, NULL};
    shown->raster.instances = &shown->instance;
    shown->raster.instance_count = 1;
    shown->raster.scratch = calloc(1, raster_scratch_bytes(&shown->raster));
    TEST_ASSERT_NOT_NULL(shown->raster.scratch);
    memset(r3d_pipeline_carve(&shown->raster).color, 0xA5, count * sizeof(uint16_t));
    return &shown->raster;
}

static void
fill_depth(const raster_t* f, uint16_t d) {
    for (int i = 0; i < f->width * f->height; i++) {
        r3d_pipeline_carve(f).depth[i] = d;
    }
}

static uint16_t*
depth_at(const raster_t* f, int x, int y) {
    return &r3d_pipeline_carve(f).depth[y * f->width + x];
}

static uint16_t
color_at(const raster_t* f, int x, int y) {
    return r3d_pipeline_carve(f).color[y * f->width + x];
}

static uint16_t
native565(uint16_t px) {
    return (uint16_t)((px >> 8) | (px << 8));
}

/* A pixel's red channel, 0..31, the panel's byte swap undone. */
static int
level(uint16_t px) {
    return native565(px) >> 11;
}

static bool
is_grey(uint16_t px) {
    const uint16_t n = native565(px);
    return (n >> 11) == (n & 31) && (n >> 11) == ((n >> 5) & 63) / 2;
}

#define LEVEL_MAX 31
#define WHITE     0xFFFF

/* Every pixel of the tile whose first pixel is (x0, y0), clipped to the raster. */
static void
assert_tile_is(const raster_t* f, int x0, int y0, uint16_t want, const char* what) {
    for (int y = y0; y < y0 + RASTER_SHOW_TILE && y < f->height; y++) {
        for (int x = x0; x < x0 + RASTER_SHOW_TILE && x < f->width; x++) {
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(want, color_at(f, x, y), what);
        }
    }
}

/* A tile shows the farthest depth in it, which is the smaller value. */
static void
test_a_tile_holds_its_minimum_depth_not_its_maximum(void) {
    raster_t* f = shown_frame(3 * RASTER_SHOW_TILE, RASTER_SHOW_TILE);
    fill_depth(f, 40000);
    for (int y = 0; y < RASTER_SHOW_TILE; y++) {
        for (int x = RASTER_SHOW_TILE; x < 2 * RASTER_SHOW_TILE; x++) {
            *depth_at(f, x, y) = 10000;
        }
    }
    *depth_at(f, 3, 3) = 10000; /* one far pixel in the first tile, the rest near */
    raster_show(f, RASTER_SHOW_DEPTH_TILES);

    const uint16_t far_tile = color_at(f, RASTER_SHOW_TILE, 0);
    const uint16_t near_tile = color_at(f, 2 * RASTER_SHOW_TILE, 0);
    TEST_ASSERT_TRUE_MESSAGE(level(far_tile) < level(near_tile), "the farther tile is not darker");
    assert_tile_is(f, 0, 0, far_tile, "a tile with one far pixel does not take that pixel's depth");
    assert_tile_is(f, 2 * RASTER_SHOW_TILE, 0, near_tile, "a uniform near tile changed");
}

/* One empty pixel, wherever in the tile, is a hole a cull must not skip. */
static void
test_one_empty_pixel_empties_its_tile_at_any_position_and_only_its_tile(void) {
    const int w = 3 * RASTER_SHOW_TILE;
    const int h = 3 * RASTER_SHOW_TILE;
    const int last = RASTER_SHOW_TILE - 1;
    const int corners[][2] = {{0, 0}, {last, last}, {last, 0}, {0, last}};
    for (int c = 0; c < 4; c++) {
        raster_t* f = shown_frame(w, h);
        fill_depth(f, 30000);
        *depth_at(f, RASTER_SHOW_TILE + corners[c][0], RASTER_SHOW_TILE + corners[c][1]) = R3D_DEPTH_EMPTY;
        raster_show(f, RASTER_SHOW_DEPTH_TILES);

        for (int ty = 0; ty < 3; ty++) {
            for (int tx = 0; tx < 3; tx++) {
                const int x0 = tx * RASTER_SHOW_TILE;
                const int y0 = ty * RASTER_SHOW_TILE;
                const bool holed = tx == 1 && ty == 1;
                TEST_ASSERT_TRUE_MESSAGE(holed == (color_at(f, x0, y0) == SKY),
                                         "an empty pixel emptied a neighbour, or missed its own tile");
                assert_tile_is(f, x0, y0, color_at(f, x0, y0), "a tile is not one colour");
            }
        }
    }
}

/* A raster that is no multiple of the tile ends in narrower and shorter
 * tiles, which are tiles like the others and read nothing outside the raster. */
static void
test_the_partial_tiles_at_the_right_and_bottom_are_reduced_within_the_frame(void) {
    const int w = (2 * RASTER_SHOW_TILE) + 4;
    const int h = RASTER_SHOW_TILE + 3;
    raster_t* f = shown_frame(w, h);
    fill_depth(f, 50000);
    *depth_at(f, w - 1, h - 1) = 20000; /* the last pixel of the corner tile */
    for (int y = 0; y < RASTER_SHOW_TILE; y++) {
        for (int x = 2 * RASTER_SHOW_TILE; x < w; x++) {
            *depth_at(f, x, y) = 60000; /* the right tile, nearest */
        }
    }
    raster_show(f, RASTER_SHOW_DEPTH_TILES);

    const uint16_t corner = color_at(f, w - 1, h - 1);
    assert_tile_is(f, 2 * RASTER_SHOW_TILE, RASTER_SHOW_TILE, corner,
                   "the corner tile did not take its last pixel's depth");
    const uint16_t right = color_at(f, w - 1, 0);
    assert_tile_is(f, 2 * RASTER_SHOW_TILE, 0, right, "the right tile is not one colour");
    TEST_ASSERT_TRUE_MESSAGE(level(corner) < level(right), "the corner tile is not the farther");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, level(corner), "the farthest tile is not black");
    for (int ty = 0; ty < 2; ty++) {
        for (int tx = 0; tx < 2; tx++) {
            const uint16_t mid = color_at(f, tx * RASTER_SHOW_TILE, ty * RASTER_SHOW_TILE);
            TEST_ASSERT_TRUE_MESSAGE(level(mid) > level(corner) && level(mid) < level(right),
                                     "a whole tile is not between the extremes");
            assert_tile_is(f, tx * RASTER_SHOW_TILE, ty * RASTER_SHOW_TILE, mid,
                           "a partial tile spilled into a whole one");
        }
    }

    *depth_at(f, w - 1, h - 1) = R3D_DEPTH_EMPTY;
    raster_show(f, RASTER_SHOW_DEPTH_TILES);
    assert_tile_is(f, 2 * RASTER_SHOW_TILE, RASTER_SHOW_TILE, SKY, "an empty pixel in a partial tile left it drawn");
    TEST_ASSERT_TRUE_MESSAGE(color_at(f, 0, h - 1) != SKY, "the empty pixel reached the bottom-left tile");
}

/* Nearest brightest, farthest darkest, whichever way the depth runs across
 * the picture. */
static void
test_the_nearest_depth_is_the_brightest_and_the_farthest_the_darkest(void) {
    for (int reversed = 0; reversed < 2; reversed++) {
        raster_t* f = shown_frame(64, 1);
        for (int x = 0; x < 64; x++) {
            *depth_at(f, x, 0) = (uint16_t)(1000 + ((reversed ? 63 - x : x) * 900));
        }
        raster_show(f, RASTER_SHOW_DEPTH);
        const int far_x = reversed ? 63 : 0;
        const int near_x = 63 - far_x;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, level(color_at(f, far_x, 0)), "the farthest pixel is not black");
        TEST_ASSERT_EQUAL_INT_MESSAGE(LEVEL_MAX, level(color_at(f, near_x, 0)), "the nearest pixel is not white");
        const int step = reversed ? -1 : 1;
        for (int x = 0; x < 63; x++) {
            TEST_ASSERT_TRUE_MESSAGE(level(color_at(f, x + 1, 0)) * step >= level(color_at(f, x, 0)) * step,
                                     "a nearer pixel is darker than a farther one");
        }
    }
}

/* Nothing to stretch over is a fixed grey, not a division by zero, and empty
 * is a colour no grey is. */
static void
test_a_frame_of_one_depth_is_one_grey_and_empty_is_no_grey(void) {
    raster_t* f = shown_frame(16, 4);
    fill_depth(f, 777);
    *depth_at(f, 5, 2) = R3D_DEPTH_EMPTY;
    raster_show(f, RASTER_SHOW_DEPTH);
    const uint16_t grey = color_at(f, 0, 0);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(WHITE, grey, "a frame of one depth is not the nearest end of the ramp");
    for (int i = 0; i < 16 * 4; i++) {
        TEST_ASSERT_EQUAL_HEX16(i == (2 * 16) + 5 ? SKY : grey, r3d_pipeline_carve(f).color[i]);
    }
    TEST_ASSERT_FALSE_MESSAGE(is_grey(SKY), "the clear colour used here is a grey");

    f = shown_frame(256, 1);
    for (int x = 0; x < 256; x++) {
        *depth_at(f, x, 0) = (uint16_t)(1 + (x * 200));
    }
    raster_show(f, RASTER_SHOW_DEPTH);
    for (int x = 0; x < 256; x++) {
        TEST_ASSERT_TRUE_MESSAGE(is_grey(color_at(f, x, 0)), "a drawn depth is not a grey");
        TEST_ASSERT_TRUE_MESSAGE(color_at(f, x, 0) != SKY, "a drawn depth reads as empty");
    }
}

/* The range the grey is stretched over is the drawn pixels'. */
static void
test_the_range_ignores_empty_pixels_and_survives_none_or_one_drawn(void) {
    raster_t* f = shown_frame(8, 8);
    *depth_at(f, 1, 1) = 30000;
    *depth_at(f, 6, 6) = 60000;
    raster_show(f, RASTER_SHOW_DEPTH);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, level(color_at(f, 1, 1)), "the empty pixels stretched the range");
    TEST_ASSERT_EQUAL_INT(LEVEL_MAX, level(color_at(f, 6, 6)));
    TEST_ASSERT_EQUAL_HEX16(SKY, color_at(f, 0, 0));

    f = shown_frame(8, 8);
    raster_show(f, RASTER_SHOW_DEPTH);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX16(SKY, r3d_pipeline_carve(f).color[i]);
    }
    raster_show(f, RASTER_SHOW_DEPTH_TILES);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_HEX16(SKY, r3d_pipeline_carve(f).color[i]);
    }

    f = shown_frame(8, 8);
    *depth_at(f, 3, 4) = 4242;
    raster_show(f, RASTER_SHOW_DEPTH);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(WHITE, color_at(f, 3, 4),
                                    "a single drawn pixel is not the nearest end of the ramp");
    TEST_ASSERT_EQUAL_HEX16(SKY, color_at(f, 4, 4));
}

/* The view of pixel i in the upscaled picture. */
static uint16_t
upscaled_at(const uint16_t* upscaled, int i) {
    return upscaled[((i / W) * 2 * 2 * W) + ((i % W) * 2)];
}

/* The views are read from the depth the render left and add nothing to it,
 * and the shaded one is the render untouched. */
static void
test_show_reads_the_depth_of_the_frame_just_rendered_and_leaves_it_alone(void) {
    parts_t* const p = parts_buffer();
    build_wall_and_stack(p);
    gfx_color_t* upscaled = malloc(sizeof(gfx_color_t) * 4 * W * H);
    char* scratch = malloc(raster_scratch_bytes(&(raster_t){ONE_MESH(&p->mesh), .width = W, .height = H,
                                                            .destination_width = 2 * W, .destination_height = 2 * H}));
    uint16_t* depth_before = malloc(sizeof(uint16_t) * W * H);
    uint16_t* shaded = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(upscaled);
    TEST_ASSERT_NOT_NULL(scratch);
    TEST_ASSERT_NOT_NULL(depth_before);
    TEST_ASSERT_NOT_NULL(shaded);
    raster_t raster = {ONE_MESH(&p->mesh),
                       .width = W,
                       .height = H,
                       .clear = SKY,
                       .upscaled = true,
                       .destination = upscaled,
                       .destination_width = 2 * W,
                       .destination_height = 2 * H};
    raster.scratch = scratch;

    static const float eye_heights[] = {-100.0f, 150.0f, 300.0f};
    uint16_t first_depth_sum = 0;
    for (int e = 0; e < 3; e++) {
        const camera_t camera = camera_down_minus_z(eye_heights[e], 400, 1.0f);
        raster_draw(&raster, &camera, 0);
        memcpy(depth_before, r3d_pipeline_carve(&raster).depth, sizeof(uint16_t) * W * H);
        memcpy(shaded, r3d_pipeline_carve(&raster).color, sizeof(uint16_t) * W * H);

        raster_show(&raster, RASTER_SHOW_SHADED);
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(shaded, r3d_pipeline_carve(&raster).color, W * H,
                                              "the shaded view changed the render");

        raster_show(&raster, RASTER_SHOW_DEPTH);
        raster_upscale(&raster);
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(depth_before, r3d_pipeline_carve(&raster).depth, W * H,
                                              "showing the depth changed it");
        int drawn = 0;
        uint16_t depth_sum = 0;
        for (int i = 0; i < W * H; i++) {
            const uint16_t d = depth_before[i];
            const uint16_t c = upscaled_at(upscaled, i);
            TEST_ASSERT_TRUE_MESSAGE((d == R3D_DEPTH_EMPTY) == (c == SKY),
                                     "empty in the depth but not in the lens, or the reverse");
            TEST_ASSERT_TRUE_MESSAGE(d == R3D_DEPTH_EMPTY || is_grey(c), "a drawn pixel is not a grey");
            drawn += d != R3D_DEPTH_EMPTY;
            depth_sum = (uint16_t)(depth_sum + d);
            if (i + 1 < W * H && (i + 1) % W != 0 && d != R3D_DEPTH_EMPTY && depth_before[i + 1] > d) {
                TEST_ASSERT_TRUE_MESSAGE(level(upscaled_at(upscaled, i + 1)) >= level(c),
                                         "the grey does not follow the depth");
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(drawn > 0, "the frame drew nothing");
        if (e == 0) {
            first_depth_sum = depth_sum;
        } else {
            TEST_ASSERT_TRUE_MESSAGE(depth_sum != first_depth_sum, "the eye moved and the depth did not");
        }
    }
    free(shaded);
    free(depth_before);
    free(scratch);
    free(upscaled);
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

static void
test_the_transform_split_is_the_shortest_prefix_holding_half_the_vertices(void) {
    const r3d_lit_cluster_t clusters[] = {
        {.vertex_count = 1}, {.vertex_count = 1}, {.vertex_count = 10}, {.vertex_count = 10}};
    const r3d_lit_mesh_t mesh = {.clusters = clusters};
    const uint16_t visible[] = {0, 1, 2, 3};
    TEST_ASSERT_EQUAL_INT(3, r3d_pipeline_transform_split(&mesh, visible, 4));
    TEST_ASSERT_EQUAL_INT(1, r3d_pipeline_transform_split(&mesh, &visible[2], 1));
    TEST_ASSERT_EQUAL_INT(0, r3d_pipeline_transform_split(&mesh, visible, 0));
}

static void
test_the_draw_split_of_no_work_is_the_middle_row(void) {
    const r3d_lit_cluster_t clusters[] = {{.triangle_count = 0}};
    const r3d_lit_mesh_t mesh = {.clusters = clusters};
    const uint16_t visible[] = {0};
    const r3d_pipeline_rows_t rows[] = {{0, 99, false}};
    TEST_ASSERT_EQUAL_INT(50, r3d_pipeline_draw_split(&mesh, visible, rows, 1, 100));
    TEST_ASSERT_EQUAL_INT(50, r3d_pipeline_draw_split(&mesh, visible, rows, 0, 100));
}

static void
test_the_draw_split_falls_inside_the_rows_the_work_covers(void) {
    const r3d_lit_cluster_t clusters[] = {{.triangle_count = 10}};
    const r3d_lit_mesh_t mesh = {.clusters = clusters};
    const uint16_t visible[] = {0};
    const r3d_pipeline_rows_t top[] = {{0, 9, false}};
    const int split = r3d_pipeline_draw_split(&mesh, visible, top, 1, 100);
    TEST_ASSERT_TRUE(split >= 1 && split <= 9);

    const r3d_pipeline_rows_t beyond[] = {{-50, 500, false}};
    const int middle = r3d_pipeline_draw_split(&mesh, visible, beyond, 1, 100);
    TEST_ASSERT_TRUE(middle >= 48 && middle <= 52);

    const r3d_pipeline_rows_t unbounded[] = {{0, 0, true}};
    TEST_ASSERT_EQUAL_INT(middle, r3d_pipeline_draw_split(&mesh, visible, unbounded, 1, 100));
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
    RUN_TEST(test_a_flat_triangle_keeps_its_face_colour_and_interpolates_depth);
    RUN_TEST(test_a_tiny_solid_triangle_takes_its_face_colour);
    RUN_TEST(test_a_window_of_rows_matches_the_same_rows_of_a_full_draw);
    RUN_TEST(test_colours_stay_within_the_vertex_range_even_at_the_edges);
    RUN_TEST(test_an_axis_aligned_square_fills_exactly_the_centres_inside_it);
    RUN_TEST(test_a_tiny_triangle_takes_the_average_of_its_corners);
    RUN_TEST(test_a_steep_sliver_puts_no_pixel_nearer_than_its_nearest_corner);
    RUN_TEST(test_every_triangle_covers_exactly_the_centres_the_top_left_rule_gives);
    RUN_TEST(test_small_shaded_triangles_keep_their_planes_in_range_in_any_window);
    RUN_TEST(test_a_mesh_of_small_and_large_triangles_fills_every_pixel_exactly_once);
    RUN_TEST(test_a_triangle_drawn_over_nearer_depth_writes_exactly_what_it_would_alone);
    RUN_TEST(test_a_guard_band_sliver_draws_its_centres_in_one_window_or_two);
    RUN_TEST(test_the_bound_is_the_greatest_depth_whichever_way_the_plane_slopes);
    RUN_TEST(test_a_span_start_clamped_up_from_below_zero_still_bounds_the_span);
    RUN_TEST(test_one_open_pixel_anywhere_in_the_box_draws_the_triangle);
    RUN_TEST(test_a_triangle_hidden_in_one_window_still_draws_in_the_other);
    RUN_TEST(test_a_plane_behind_a_wall_is_hidden_and_one_reaching_past_it_is_not);
    RUN_TEST(test_a_plane_below_zero_at_a_corner_is_bounded_by_its_lift);
    RUN_TEST(test_a_plane_is_in_range_exactly_when_every_centre_of_its_box_is);
    RUN_TEST(test_triangles_behind_a_nearer_wall_leave_colour_and_depth_untouched);
    RUN_TEST(test_a_window_holding_a_few_rows_of_a_tall_sliver_draws_them_as_the_whole_does);
    RUN_TEST(test_a_triangle_with_corners_past_the_snap_range_fills_its_centres);
    RUN_TEST(test_a_triangle_whose_only_centre_is_at_a_window_edge_is_drawn);
    RUN_TEST(test_rebuilt_and_fast_triangles_sharing_edges_fill_every_pixel_once);

    RUN_TEST(test_a_counter_clockwise_face_toward_the_camera_is_drawn);
    RUN_TEST(test_a_face_turned_away_is_culled_unless_double_sided);
    RUN_TEST(test_a_cluster_behind_the_camera_is_culled);
    RUN_TEST(test_a_floor_crossing_the_near_plane_draws_only_below_the_horizon);
    RUN_TEST(test_a_mesh_with_face_colours_draws_each_triangle_in_its_own);
    RUN_TEST(test_face_colours_survive_a_triangle_clipped_by_the_near_plane);
    RUN_TEST(test_a_point_up_and_right_lands_up_and_right_in_every_quarter);
    RUN_TEST(test_a_triangle_cut_by_the_near_plane_draws_the_whole_quad_left_in_front);
    RUN_TEST(test_colour_along_the_near_cut_is_interpolated_to_the_cut);
    RUN_TEST(test_a_double_sided_triangle_cut_by_the_near_plane_draws_from_behind);
    RUN_TEST(test_drawing_a_window_with_cluster_rows_matches_a_full_draw);
    RUN_TEST(test_a_triangle_over_any_side_of_the_screen_is_drawn);

    RUN_TEST(test_the_frame_carves_its_scratch_without_overlap);
    RUN_TEST(test_the_two_core_frame_matches_one_full_draw);
    RUN_TEST(test_a_destination_of_the_same_size_is_a_copy);
    RUN_TEST(test_a_fractional_destination_upscales_a_drawn_frame);
    RUN_TEST(test_a_tile_holds_its_minimum_depth_not_its_maximum);
    RUN_TEST(test_one_empty_pixel_empties_its_tile_at_any_position_and_only_its_tile);
    RUN_TEST(test_the_partial_tiles_at_the_right_and_bottom_are_reduced_within_the_frame);
    RUN_TEST(test_the_nearest_depth_is_the_brightest_and_the_farthest_the_darkest);
    RUN_TEST(test_a_frame_of_one_depth_is_one_grey_and_empty_is_no_grey);
    RUN_TEST(test_the_range_ignores_empty_pixels_and_survives_none_or_one_drawn);
    RUN_TEST(test_show_reads_the_depth_of_the_frame_just_rendered_and_leaves_it_alone);
    RUN_TEST(test_the_transform_split_is_the_shortest_prefix_holding_half_the_vertices);
    RUN_TEST(test_the_draw_split_of_no_work_is_the_middle_row);
    RUN_TEST(test_the_draw_split_falls_inside_the_rows_the_work_covers);
}

#undef RUN_TEST

SUITE_REGISTER(run_r3d_lit_suite);
