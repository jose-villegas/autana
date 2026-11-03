

#include "suites.h"
#include "unity.h"

#include "gfx/target/gfx_target.h"

#include <string.h>

#define WIDTH  8
#define HEIGHT 8

static gfx_color_t full_buf[WIDTH * HEIGHT];

static const gfx_target_t*
full_target(void) {
    static const GFX_TARGET_STORAGE(1) storage = GFX_TARGET_COLOR_INIT(full_buf, WIDTH, HEIGHT);
    return &storage.target;
}

static void
fixture(void) {
    memset(full_buf, 0, sizeof full_buf);
}

/* clipping and translation, one target */

static void
test_a_rect_entirely_inside_the_target_is_untouched_by_clipping(void) {
    fixture();

    const recti_t painted =
        gfx_target_fill_rect(full_target(), (recti_t){0, 0, WIDTH, HEIGHT}, (recti_t){2, 2, 3, 3}, (gfx_color_t)0xFFFF);

    TEST_ASSERT_EQUAL_INT(2, painted.x);
    TEST_ASSERT_EQUAL_INT(2, painted.y);
    TEST_ASSERT_EQUAL_INT(5, recti_right(painted));
    TEST_ASSERT_EQUAL_INT(5, recti_bottom(painted));
    for (int y = 2; y < 5; y++) {
        for (int x = 2; x < 5; x++) {
            TEST_ASSERT_EQUAL_HEX16(0xFFFF, full_buf[y * WIDTH + x]);
        }
    }
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, full_buf[1 * WIDTH + 2], "the rect must not touch its neighbours");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, full_buf[5 * WIDTH + 2], "the rect must not touch its neighbours");
}

static void
test_the_targets_own_row_range_narrows_a_wider_clip(void) {
    GFX_TARGET_STORAGE(1) band_storage;
    gfx_target_of_color(&band_storage.target, full_buf, WIDTH, 3, 2);
    const gfx_target_t* band = &band_storage.target; /* rows [2, 5) only */

    const recti_t rect = gfx_target_clip_rect(band, (recti_t){0, 0, WIDTH, HEIGHT}, (recti_t){0, 0, WIDTH, 8});

    TEST_ASSERT_EQUAL_INT(2, rect.y);
    TEST_ASSERT_EQUAL_INT(5, recti_bottom(rect));
}

/* The app's own clip rect must win when it is narrower than the target. */
static void
test_the_apps_clip_narrows_a_wider_target(void) {
    const gfx_target_t* full = full_target();

    const recti_t rect = gfx_target_clip_rect(full, (recti_t){0, 3, WIDTH, 3}, (recti_t){0, 0, WIDTH, 8});

    TEST_ASSERT_EQUAL_INT(3, rect.y);
    TEST_ASSERT_EQUAL_INT(6, recti_bottom(rect));
}

static void
test_row_translation_lands_in_the_bands_own_local_row(void) {
    gfx_color_t band_buf[WIDTH * 3] = {0};
    GFX_TARGET_STORAGE(1) band_storage;
    gfx_target_of_color(&band_storage.target, band_buf, WIDTH, 3, 4);
    const gfx_target_t* band = &band_storage.target; /* absolute rows [4, 7) */

    gfx_target_color(band, 5)[0] = (gfx_color_t)0xABCD;

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xABCD, band_buf[1 * WIDTH + 0],
                                    "absolute row 5 must land in the band's own local row 1 (5 - row0 4)");
}

/* straddling a band edge */

static void
test_a_rect_straddling_a_band_edge_splits_correctly(void) {
    gfx_color_t band_a[WIDTH * 4] = {0}; /* absolute rows [0, 4) */
    gfx_color_t band_b[WIDTH * 4] = {0}; /* absolute rows [4, 8) */
    const GFX_TARGET_STORAGE(1) a_storage = GFX_TARGET_COLOR_INIT(band_a, WIDTH, 4);
    const gfx_target_t* a = &a_storage.target;
    GFX_TARGET_STORAGE(1) b_storage;
    gfx_target_of_color(&b_storage.target, band_b, WIDTH, 4, 4);
    const gfx_target_t* b = &b_storage.target;

    /* A column at x=3, rows [2, 6), straddles the row-4 band edge. */

    const recti_t apainted =
        gfx_target_fill_rect(a, (recti_t){0, 0, WIDTH, HEIGHT}, (recti_t){3, 2, 1, 4}, (gfx_color_t)0x1111);
    const recti_t bpainted =
        gfx_target_fill_rect(b, (recti_t){0, 0, WIDTH, HEIGHT}, (recti_t){3, 2, 1, 4}, (gfx_color_t)0x1111);

    TEST_ASSERT_EQUAL_INT(2, apainted.y);
    TEST_ASSERT_EQUAL_INT(4, recti_bottom(apainted)); /* band a only got its own two rows */
    TEST_ASSERT_EQUAL_INT(4, bpainted.y);
    TEST_ASSERT_EQUAL_INT(6, recti_bottom(bpainted)); /* band b got the other two */

    TEST_ASSERT_EQUAL_HEX16(0x1111, band_a[2 * WIDTH + 3]);
    TEST_ASSERT_EQUAL_HEX16(0x1111, band_a[3 * WIDTH + 3]);
    TEST_ASSERT_EQUAL_HEX16(0x1111, band_b[0 * WIDTH + 3]); /* local row 0 == absolute row 4 */
    TEST_ASSERT_EQUAL_HEX16(0x1111, band_b[1 * WIDTH + 3]); /* local row 1 == absolute row 5 */
}

/* the union of every band equals one full-target render */

static void
test_the_union_of_all_bands_equals_the_full_target_render(void) {
    fixture();

    typedef struct {
        int x, y, w, h;
        gfx_color_t color;
    } shape_t;

    const shape_t shapes[] = {
        {0, 0, 8, 8, (gfx_color_t)0x2222}, /* a full-screen background */
        {1, 1, 4, 3, (gfx_color_t)0x3333}, /* an overlapping box */
        {3, 0, 1, 8, (gfx_color_t)0x4444}, /* a full-height column, like a glyph stroke */
    };
    const int shape_count = (int)(sizeof shapes / sizeof shapes[0]);

    for (int i = 0; i < shape_count; i++) {
        (void)gfx_target_fill_rect(full_target(), (recti_t){0, 0, WIDTH, HEIGHT},
                                   (recti_t){shapes[i].x, shapes[i].y, shapes[i].w, shapes[i].h}, shapes[i].color);
    }

    gfx_color_t band_buf[4][WIDTH * 2] = {0}; /* 4 bands of 2 rows each = 8 rows */
    for (int band = 0; band < 4; band++) {
        GFX_TARGET_STORAGE(1) target_storage;
        gfx_target_of_color(&target_storage.target, band_buf[band], WIDTH, 2, band * 2);
        const gfx_target_t* target = &target_storage.target;
        for (int i = 0; i < shape_count; i++) {
            if (!gfx_target_row_range_overlaps(target, shapes[i].y, shapes[i].y + shapes[i].h)) {
                continue; /* the per-band skip every replay path relies on */
            }

            (void)gfx_target_fill_rect(target, (recti_t){0, 0, WIDTH, HEIGHT},
                                       (recti_t){shapes[i].x, shapes[i].y, shapes[i].w, shapes[i].h}, shapes[i].color);
        }
    }

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            const gfx_color_t want = full_buf[y * WIDTH + x];
            const gfx_color_t got = band_buf[y / 2][(y % 2) * WIDTH + x];
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(want, got,
                                            "banded render must match the full-target render pixel for pixel");
        }
    }
}

static void
test_row_range_overlap_rejects_a_shape_the_band_never_reaches(void) {
    GFX_TARGET_STORAGE(1) band_storage;
    gfx_target_of_color(&band_storage.target, full_buf, WIDTH, 2, 6);
    const gfx_target_t* band = &band_storage.target; /* rows [6, 8) */

    TEST_ASSERT_FALSE(gfx_target_row_range_overlaps(band, 0, 4));
    TEST_ASSERT_TRUE(gfx_target_row_range_overlaps(band, 5, 7));
    TEST_ASSERT_TRUE_MESSAGE(gfx_target_row_range_overlaps(band, 0, 8), "a full-height shape overlaps every band");
}

void
run_gfx_target_suite(void) {
    RUN_TEST(test_a_rect_entirely_inside_the_target_is_untouched_by_clipping);
    RUN_TEST(test_the_targets_own_row_range_narrows_a_wider_clip);
    RUN_TEST(test_the_apps_clip_narrows_a_wider_target);
    RUN_TEST(test_row_translation_lands_in_the_bands_own_local_row);
    RUN_TEST(test_a_rect_straddling_a_band_edge_splits_correctly);
    RUN_TEST(test_the_union_of_all_bands_equals_the_full_target_render);
    RUN_TEST(test_row_range_overlap_rejects_a_shape_the_band_never_reaches);
}

SUITE_REGISTER(run_gfx_target_suite);
