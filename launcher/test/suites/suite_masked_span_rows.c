/*
 * Portable suite: a masked span fill reads the atlas row and column each pixel's
 * u, v picks. A card is drawn as two triangles whose u, v run affinely over the
 * screen; every pixel is checked against that plane sampled at its centre, with
 * and without span writers. The checks hold the fill's row splitting, its
 * clamps at the atlas edges, its window and its writer runs to one model.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565.h"
#include "render/lmsk_format.h"
#include "render/r3d_span_internal.h"
#include "suites.h"
#include "unity.h"

#define CANVAS_WIDTH   40
#define CANVAS_HEIGHT  20
#define CANVAS_PIXELS  (CANVAS_WIDTH * CANVAS_HEIGHT)
#define CANVAS_MAX     4
#define ATLAS_ROWS     32
#define ATLAS_ROW_HASH UINT32_C(0x9E3779B1) /* each row a different, mixed word */
#define CARD_CORNERS   4
#define CARD_TRIANGLES 2
#define CARD_COLOR     RGB565BE(0xFF0000)
#define CARD_DEPTH     ((uint16_t)(R3D_DEPTH_NEAREST / 2))
#define CARD_Z         0.5F
#define CARD_RED       UINT8_MAX
#define UV_UNIT        (1 << LMSK_UV_FRACTION_BITS)
#define DEPTH_KINDS    3 /* before a draw: nearer than the card, equal to it, empty */
#define PIXEL_CENTRE   0.5F
/* A plane that is not flat must never sample within this of a texel boundary: fixed point could tip it. */
#define EDGE_MARGIN    (1.0F / 256.0F)

typedef struct {
    uint16_t color[CANVAS_PIXELS];
    uint16_t depth[CANVAS_PIXELS];
    uint16_t hits[CANVAS_PIXELS]; /* times a writer was handed the pixel */
    int bad_depth;                /* writer pixels whose depth is not the one the draw left */
} canvas_t;

static canvas_t* owned[CANVAS_MAX];
static uint32_t* atlas_rows;
static canvas_t* sink;

static void canvases_free(void);

static void
cleanup(void) {
    canvases_free();
    free(atlas_rows);
    atlas_rows = NULL;
    sink = NULL;
}

static void
canvases_free(void) {
    for (int i = 0; i < CANVAS_MAX; i++) {
        free(owned[i]);
        owned[i] = NULL;
    }
}

static canvas_t*
canvas_new(void) {
    suite_set_test_cleanup(cleanup);
    for (int i = 0; i < CANVAS_MAX; i++) {
        if (owned[i] == NULL) {
            owned[i] = calloc(1, sizeof(canvas_t));
            TEST_ASSERT_NOT_NULL(owned[i]);
            return owned[i];
        }
    }
    TEST_FAIL_MESSAGE("a test asked for more canvases than CANVAS_MAX");
    return NULL;
}

/* An atlas of `row_count` mixed rows, owned until the test ends. */
static lmsk_atlas_t
atlas_new(int row_count) {
    suite_set_test_cleanup(cleanup);
    free(atlas_rows);
    atlas_rows = malloc(sizeof(*atlas_rows) * (size_t)row_count);
    TEST_ASSERT_NOT_NULL(atlas_rows);
    for (int r = 0; r < row_count; r++) {
        atlas_rows[r] = (uint32_t)(r + 1) * ATLAS_ROW_HASH;
    }
    return (lmsk_atlas_t){atlas_rows, row_count};
}

/* A rectangle of pixels, possibly off the target, and its u, v in texels: their value at its top-left
 * corner and their step per pixel along x and y. z and rgb are per corner: top-left, top-right,
 * bottom-right, bottom-left. */
typedef struct {
    int x0, y0, x1, y1;
    float u0, ux, uy, v0, vx, vy;
    float z[CARD_CORNERS];
    float rgb[CARD_CORNERS][3];
} card_t;

static card_t
card_of(int x0, int y0, int x1, int y1, const float u[3], const float v[3]) {
    card_t c = {
        .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .u0 = u[0], .ux = u[1], .uy = u[2], .v0 = v[0], .vx = v[1], .vy = v[2]};
    for (int k = 0; k < CARD_CORNERS; k++) {
        c.z[k] = CARD_Z;
        c.rgb[k][0] = CARD_RED;
    }
    return c;
}

/* The plane's texel at a pixel centre. */
static float
texel_at(float at_corner, float along_x, float along_y, const card_t* c, int x, int y) {
    return at_corner + (along_x * ((float)(x - c->x0) + PIXEL_CENTRE))
           + (along_y * ((float)(y - c->y0) + PIXEL_CENTRE));
}

static int
clamped(float texel, int count) {
    const int at = (int)floorf(texel);
    return at < 0 ? 0 : at >= count ? count - 1 : at;
}

static bool
keeps(const lmsk_atlas_t* atlas, const card_t* c, int x, int y) {
    const float u = texel_at(c->u0, c->ux, c->uy, c, x, y);
    const float v = texel_at(c->v0, c->vx, c->vy, c, x, y);
    return ((atlas->rows[clamped(v, atlas->row_count)] >> clamped(u, (int)LMSK_MASK_WIDTH)) & 1U) != 0;
}

/* A case that samples a boundary between texels on a moving plane would test fixed-point rounding, not the fill. */
static void
assert_clear_of_texel_edges(const card_t* c) {
    for (int y = c->y0; y < c->y1; y++) {
        for (int x = c->x0; x < c->x1; x++) {
            const float u = texel_at(c->u0, c->ux, c->uy, c, x, y);
            const float v = texel_at(c->v0, c->vx, c->vy, c, x, y);
            if (c->ux != 0 || c->uy != 0) {
                TEST_ASSERT_GREATER_THAN_FLOAT(EDGE_MARGIN, fabsf(u - roundf(u)));
            }
            if (c->vx != 0 || c->vy != 0) {
                TEST_ASSERT_GREATER_THAN_FLOAT(EDGE_MARGIN, fabsf(v - roundf(v)));
            }
        }
    }
}

static void
draw_card(const r3d_span_target_t* target, const lmsk_atlas_t* atlas, const card_t* c, const uint16_t* face,
          bool opaque) {
    const int xs[CARD_CORNERS] = {c->x0, c->x1, c->x1, c->x0};
    const int ys[CARD_CORNERS] = {c->y0, c->y0, c->y1, c->y1};
    r3d_span_vertex_t quad[CARD_CORNERS];
    float u[CARD_CORNERS];
    float v[CARD_CORNERS];
    for (int k = 0; k < CARD_CORNERS; k++) {
        quad[k] = (r3d_span_vertex_t){xs[k] * R3D_SUBPIXEL, ys[k] * R3D_SUBPIXEL, c->z[k],
                                      c->rgb[k][0],         c->rgb[k][1],         c->rgb[k][2]};
        u[k] = (c->u0 + (c->ux * (float)(xs[k] - c->x0)) + (c->uy * (float)(ys[k] - c->y0))) * UV_UNIT;
        v[k] = (c->v0 + (c->vx * (float)(xs[k] - c->x0)) + (c->vy * (float)(ys[k] - c->y0))) * UV_UNIT;
    }
    static const int corners[CARD_TRIANGLES][R3D_SPAN_TRIANGLE_VERTICES] = {{0, 1, 2}, {0, 2, 3}};
    for (int t = 0; t < CARD_TRIANGLES; t++) {
        const r3d_span_vertex_t *a = &quad[corners[t][0]], *b = &quad[corners[t][1]], *d = &quad[corners[t][2]];
        if (opaque) {
            if (face != NULL) {
                r3d_span_triangle_solid(target, a, b, d, *face);
            } else {
                r3d_span_triangle(target, a, b, d);
            }
            continue;
        }
        r3d_span_mask_t mask = {atlas, {0}, {0}};
        for (int k = 0; k < R3D_SPAN_TRIANGLE_VERTICES; k++) {
            mask.u[k] = u[corners[t][k]];
            mask.v[k] = v[corners[t][k]];
        }
        r3d_span_masked_triangle(target, &mask, a, b, d, face);
    }
}

static void
record_written(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int first, int last, int32_t z,
               int32_t dz) {
    for (int x = first; x <= last; x++) {
        const int at = (y * CANVAS_WIDTH) + x;
        sink->hits[at]++;
        sink->bad_depth += (uint16_t)((z + (dz * (x - first))) >> R3D_DEPTH_SHIFT) != sink->depth[at];
    }
}

/* Depth before a draw, cycling nearer than the card (it loses), equal (a tie loses) and empty (it wins). */
static uint16_t
depth_before(int i) {
    const int kind = i % DEPTH_KINDS;
    return kind == 0 ? R3D_DEPTH_NEAREST : kind == 1 ? CARD_DEPTH : R3D_DEPTH_EMPTY;
}

/* Rows [row0, row1) of the card drawn into a fresh canvas: colour and depth alone over empty depth, and with a
 * writer over depth the card only partly wins. A pixel is changed only inside the window and the card, where the
 * atlas keeps it, and, with the writer, where depth lets it win; and a writer is handed exactly those pixels. */
static void
check_card_in_rows(const card_t* c, const lmsk_atlas_t* atlas, int row0, int row1) {
    assert_clear_of_texel_edges(c);
    for (int writing = 0; writing < 2; writing++) {
        canvases_free();
        canvas_t* cv = canvas_new();
        for (int i = 0; i < CANVAS_PIXELS; i++) {
            cv->depth[i] = writing != 0 ? depth_before(i) : R3D_DEPTH_EMPTY;
        }
        r3d_span_target_t target = r3d_span_target(cv->color + (row0 * CANVAS_WIDTH), cv->depth + (row0 * CANVAS_WIDTH),
                                                   CANVAS_WIDTH, row0, row1);
        const r3d_span_writer_t writer = {.span = record_written};
        if (writing != 0) {
            target.writers = &writer;
            target.writer_count = 1;
            sink = cv;
        }
        draw_card(&target, atlas, c, NULL, false);
        for (int y = 0; y < CANVAS_HEIGHT; y++) {
            for (int x = 0; x < CANVAS_WIDTH; x++) {
                const int i = (y * CANVAS_WIDTH) + x;
                const bool inside = y >= row0 && y < row1 && x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1;
                const bool wins = writing == 0 || depth_before(i) == R3D_DEPTH_EMPTY;
                const bool drawn = inside && wins && keeps(atlas, c, x, y);
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(drawn ? CARD_COLOR : 0, cv->color[i], "colour");
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(
                    drawn ? CARD_DEPTH : (writing != 0 ? depth_before(i) : R3D_DEPTH_EMPTY), cv->depth[i], "depth");
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(writing != 0 && drawn ? 1 : 0, cv->hits[i], "writer hits");
            }
        }
        TEST_ASSERT_EQUAL_INT(0, cv->bad_depth);
    }
}

static void
check_card(const card_t* c, const lmsk_atlas_t* atlas) {
    check_card_in_rows(c, atlas, 0, CANVAS_HEIGHT);
}

/* The planes below are given as {texel at the corner, step along x, step along y}. */

/* With v falling along x a span passes down through the rows: each is read for the pixels it holds. */
static void
test_masked_span_with_v_falling_along_x_reads_each_row(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    const card_t c = card_of(2, 3, 34, 11, (float[]){0, 0.5F, 1.0F}, (float[]){24.0F, -0.5F, 0.25F});
    check_card(&c, &atlas);
}

/* A row boundary falls mid-span, at a different pixel on every row, rising or falling. */
static void
test_masked_span_reads_a_new_row_part_way_along_when_v_steps_a_fraction_of_a_row(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    static const float steps[] = {0.375F, -0.375F};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        const card_t c = card_of(4, 2, 34, 12, (float[]){1.0F, 0.75F, 0.0625F}, (float[]){12.0F, steps[i], 0.0625F});
        check_card(&c, &atlas);
    }
}

/* More than a row a pixel: every piece of a span is one or a few pixels. */
static void
test_masked_span_reads_a_new_row_each_pixel_when_v_steps_over_a_row(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    const card_t up = card_of(4, 2, 16, 12, (float[]){0.5F, 0.5F, 0.0625F}, (float[]){1.0F, 2.5F, 0.0625F});
    check_card(&up, &atlas);
    const card_t down = card_of(4, 2, 16, 12, (float[]){0.5F, 0.5F, 0.0625F}, (float[]){31.0F, -2.5F, 0.0625F});
    check_card(&down, &atlas);
}

/* A span of one pixel is a whole row of its own: it reads the row its v picks. */
static void
test_masked_span_of_one_pixel_reads_its_row(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    const card_t c = card_of(5, 2, 6, 10, (float[]){3.0F, 0.5F, 1.125F}, (float[]){20.0F, -0.75F, 0.5F});
    check_card(&c, &atlas);
}

/* With no step of v along x each span reads the one row its y picks, however far u runs. */
static void
test_masked_span_with_no_v_step_reads_one_row(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    const card_t c = card_of(2, 2, 34, 10, (float[]){0.0F, 0.75F, 0.0F}, (float[]){2.0F, 0.0F, 0.75F});
    check_card(&c, &atlas);
}

/* u that stays put on a span past either edge, or exactly on one, reads the first or the last column. */
static void
test_masked_span_clamps_u_to_the_first_and_last_column(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    static const float edge_u[] = {-100.0F, -0.0625F, 0.0F, 31.9375F, 32.0F, 500.0F};
    for (size_t i = 0; i < sizeof edge_u / sizeof edge_u[0]; i++) {
        const card_t c = card_of(2, 2, 30, 10, (float[]){edge_u[i], 0, 0}, (float[]){1.0F, 0.5F, 0.75F});
        check_card(&c, &atlas);
    }
}

/* u wholly past an edge and moving on, or away, reads that edge's column all the way. */
static void
test_masked_span_with_u_moving_past_an_edge_reads_the_edge_column(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    const card_t beyond_last = card_of(2, 2, 30, 10, (float[]){40.0F, 0.5F, 0.0625F}, (float[]){1.0F, 0.1875F, 0.75F});
    check_card(&beyond_last, &atlas);
    const card_t before_first =
        card_of(2, 2, 30, 10, (float[]){-30.0F, -0.5F, 0.0625F}, (float[]){1.0F, 0.1875F, 0.75F});
    check_card(&before_first, &atlas);
}

/* v on a span past either edge, or exactly on one, reads the first or the last row; v wholly past an edge
 * and moving on reads that row all the way: v is clamped once per span, as u is. */
static void
test_masked_span_clamps_v_to_the_first_and_last_row(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    static const float edge_v[] = {-100.0F, -0.0625F, 0.0F, 31.9375F, 32.0F, 500.0F};
    for (size_t i = 0; i < sizeof edge_v / sizeof edge_v[0]; i++) {
        const card_t c = card_of(2, 2, 30, 10, (float[]){1.0F, 0.5F, 0.75F}, (float[]){edge_v[i], 0, 0});
        check_card(&c, &atlas);
    }
    const card_t past_the_last = card_of(2, 2, 30, 10, (float[]){1.0F, 0.5F, 0.75F}, (float[]){40.0F, 0.375F, 0.0625F});
    check_card(&past_the_last, &atlas);
    const card_t before_the_first =
        card_of(2, 2, 30, 10, (float[]){1.0F, 0.5F, 0.75F}, (float[]){-30.0F, -0.375F, 0.0625F});
    check_card(&before_the_first, &atlas);
}

/* An atlas of fewer rows than the card's v runs over: the last row serves every v past it. */
static void
test_masked_span_over_an_atlas_shorter_than_the_card_reads_its_last_row(void) {
    enum { SHORT_ROWS = 3 };

    const lmsk_atlas_t atlas = atlas_new(SHORT_ROWS);
    const card_t flat = card_of(2, 2, 34, 10, (float[]){0.0F, 0.5F, 1.0F}, (float[]){5.0625F, 0.0F, 0.25F});
    check_card(&flat, &atlas);
    const card_t moving_on = card_of(2, 2, 34, 10, (float[]){0.0F, 0.5F, 1.0F}, (float[]){5.0625F, 0.5F, 0.25F});
    check_card(&moving_on, &atlas);
}

/* The span starts left of x = 0: its first visible pixel reads the texel the plane has there, not the card's
 * first. */
static void
test_masked_span_starting_left_of_the_target_reads_the_texels_where_it_is_seen(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    const card_t c = card_of(-9, 3, 25, 11, (float[]){0.0F, 0.75F, 0.0625F}, (float[]){24.0F, -0.5F, 0.25F});
    check_card(&c, &atlas);
}

/* A target of rows [first, end) alone: the planes still run from the card's own rows. */
static void
test_masked_card_over_a_row_window_changes_only_the_window(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);

    enum { WINDOW_FIRST = 6, WINDOW_END = 13 };

    const card_t c = card_of(2, 2, 34, 18, (float[]){0.0F, 0.5F, 0.375F}, (float[]){26.0F, -0.375F, 0.5F});
    check_card_in_rows(&c, &atlas, WINDOW_FIRST, WINDOW_END);
}

/* With writers each kept run is filled and handed on: where v crosses rows inside a span the runs break
 * at the crossing, and no pixel is handed on twice or one the atlas drops. */
static void
test_masked_writer_runs_hold_across_row_crossings(void) {
    enum { WRITER_SHORT_RIGHT = 13, WRITER_LONG_RIGHT = 37 };

    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    static const float steps[] = {-0.5F, 0.375F, -2.5F, 2.5F};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        const int right =
            fabsf(steps[i]) > 1.0F ? WRITER_SHORT_RIGHT : WRITER_LONG_RIGHT; /* v stays inside the atlas */
        const card_t c = card_of(1, 2, right, 12, (float[]){0.0F, 0.75F, 0.0625F},
                                 (float[]){steps[i] < 0 ? 31.0F : 1.0F, steps[i], 0.0625F});
        check_card(&c, &atlas);
    }
}

/* Gouraud vertex colours and depth, as the opaque fill draws them. */
static card_t
varying_card(void) {
    card_t c = card_of(3, 2, 29, 15, (float[]){0.0F, 1.0F, 0.0625F}, (float[]){2.0F, 0.375F, 1.0F});
    static const float z[CARD_CORNERS] = {0.9F, 0.3F, 0.6F, 0.45F};
    static const float rgb[CARD_CORNERS][3] = {{250, 20, 40}, {30, 240, 60}, {20, 40, 250}, {200, 200, 30}};
    memcpy(c.z, z, sizeof z);
    memcpy(c.rgb, rgb, sizeof rgb);
    return c;
}

static void
assert_same_as_opaque(const card_t* c, const uint16_t* face, bool skip_hidden) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    assert_clear_of_texel_edges(c);
    canvases_free();
    canvas_t* opaque = canvas_new();
    canvas_t* masked = canvas_new();
    r3d_span_target_t reference = r3d_span_target(opaque->color, opaque->depth, CANVAS_WIDTH, 0, CANVAS_HEIGHT);
    r3d_span_target_t target = r3d_span_target(masked->color, masked->depth, CANVAS_WIDTH, 0, CANVAS_HEIGHT);
    reference.skip_hidden = skip_hidden;
    target.skip_hidden = skip_hidden;
    draw_card(&reference, &atlas, c, face, true);
    draw_card(&target, &atlas, c, face, false);
    int kept = 0;
    for (int y = 0; y < CANVAS_HEIGHT; y++) {
        for (int x = 0; x < CANVAS_WIDTH; x++) {
            const int i = (y * CANVAS_WIDTH) + x;
            const bool inside = x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1;
            if (inside && keeps(&atlas, c, x, y)) {
                kept++;
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(opaque->color[i], masked->color[i], "kept colour");
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(opaque->depth[i], masked->depth[i], "kept depth");
            } else {
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, masked->color[i], "dropped colour");
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(R3D_DEPTH_EMPTY, masked->depth[i], "dropped depth");
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, kept, "the card kept nothing");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE((c->x1 - c->x0) * (c->y1 - c->y0), kept, "the card kept everything");
}

/* The mask only drops pixels: those it keeps get the colour and depth the opaque Gouraud fill gives them. */
static void
test_masked_fill_keeps_the_gouraud_colour_and_depth_of_the_opaque_fill(void) {
    const card_t c = varying_card();
    assert_same_as_opaque(&c, NULL, false);
}

/* A scene that skips hidden clusters caps the fills' depth at their nearest vertex's. The cap only ever
 * guards against a plane's rounding past a vertex, so a card whose depth is steep, here from near to far
 * in a few pixels, still draws the same colour and depth with it as without. */
static void
test_masked_fill_draws_the_same_pixels_when_hidden_clusters_are_skipped(void) {
    const lmsk_atlas_t atlas = atlas_new(ATLAS_ROWS);
    static const float steep_z[CARD_CORNERS] = {0.98F, 0.02F, 0.02F, 0.98F};
    static const int steep_widths[] = {1, 2, 6, 12};
    for (size_t w = 0; w < sizeof steep_widths / sizeof steep_widths[0]; w++) {
        card_t c =
            card_of(3, 2, 3 + steep_widths[w], 15, (float[]){0.0F, 1.0F, 0.0625F}, (float[]){2.0F, 0.375F, 1.0F});
        memcpy(c.z, steep_z, sizeof steep_z);
        assert_clear_of_texel_edges(&c);
        canvases_free();
        canvas_t* views[2];
        for (int skipping = 0; skipping < 2; skipping++) {
            views[skipping] = canvas_new();
            r3d_span_target_t target =
                r3d_span_target(views[skipping]->color, views[skipping]->depth, CANVAS_WIDTH, 0, CANVAS_HEIGHT);
            target.skip_hidden = skipping != 0;
            draw_card(&target, &atlas, &c, NULL, false);
        }
        int drawn = 0;
        for (int i = 0; i < CANVAS_PIXELS; i++) {
            TEST_ASSERT_EQUAL_HEX16(views[0]->color[i], views[1]->color[i]);
            TEST_ASSERT_EQUAL_UINT16(views[0]->depth[i], views[1]->depth[i]);
            drawn += views[0]->depth[i] != R3D_DEPTH_EMPTY;
        }
        TEST_ASSERT_GREATER_THAN_INT(0, drawn);
    }
    const card_t gentle = varying_card();
    canvases_free();
    canvas_t* views[2];
    for (int skipping = 0; skipping < 2; skipping++) {
        views[skipping] = canvas_new();
        r3d_span_target_t target =
            r3d_span_target(views[skipping]->color, views[skipping]->depth, CANVAS_WIDTH, 0, CANVAS_HEIGHT);
        target.skip_hidden = skipping != 0;
        draw_card(&target, &atlas, &gentle, NULL, false);
    }
    TEST_ASSERT_EQUAL_MEMORY(views[0]->color, views[1]->color, sizeof views[0]->color);
    TEST_ASSERT_EQUAL_MEMORY(views[0]->depth, views[1]->depth, sizeof views[0]->depth);
}

void
run_masked_span_rows_suite(void) {
    RUN_TEST(test_masked_span_with_v_falling_along_x_reads_each_row);
    RUN_TEST(test_masked_span_reads_a_new_row_part_way_along_when_v_steps_a_fraction_of_a_row);
    RUN_TEST(test_masked_span_reads_a_new_row_each_pixel_when_v_steps_over_a_row);
    RUN_TEST(test_masked_span_of_one_pixel_reads_its_row);
    RUN_TEST(test_masked_span_with_no_v_step_reads_one_row);
    RUN_TEST(test_masked_span_clamps_u_to_the_first_and_last_column);
    RUN_TEST(test_masked_span_with_u_moving_past_an_edge_reads_the_edge_column);
    RUN_TEST(test_masked_span_clamps_v_to_the_first_and_last_row);
    RUN_TEST(test_masked_span_over_an_atlas_shorter_than_the_card_reads_its_last_row);
    RUN_TEST(test_masked_span_starting_left_of_the_target_reads_the_texels_where_it_is_seen);
    RUN_TEST(test_masked_card_over_a_row_window_changes_only_the_window);
    RUN_TEST(test_masked_writer_runs_hold_across_row_crossings);
    RUN_TEST(test_masked_fill_keeps_the_gouraud_colour_and_depth_of_the_opaque_fill);
    RUN_TEST(test_masked_fill_draws_the_same_pixels_when_hidden_clusters_are_skipped);
}

SUITE_REGISTER(run_masked_span_rows_suite);
