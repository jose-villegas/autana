#include <stdlib.h>
#include "image/pixel/rgb565.h"
#include "render/contact/contact_blob.h"
#include "suites.h"
#include "unity.h"

enum { BLOB_WIDTH = 9, BLOB_HEIGHT = 9, BLOB_CENTRE = 4, BLOB_DEPTH = 4 };

#define BLOB_RADIUS 1.0F

static void
test_blob_depth_radius_rows_strength(void) {
    uint16_t* color = malloc(BLOB_WIDTH * BLOB_HEIGHT * sizeof *color);
    uint16_t* depth = calloc(BLOB_WIDTH * BLOB_HEIGHT, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    for (int i = 0; i < BLOB_WIDTH * BLOB_HEIGHT; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    const transformf_t pose = transformf_identity();
    render_view_t view = render_view_make(&pose, 1, 1, (viewport_t){BLOB_WIDTH, BLOB_HEIGHT, 0});
    r3d_lens_t lens;
    r3d_lens_init(&lens, &view, 1);
    /* The centre ray passes through the plane's centre at this depth. */
    const vec3f_t ground = {(BLOB_CENTRE + 0.5F - lens.project.cx) * BLOB_DEPTH / lens.focal_x,
                            -(BLOB_CENTRE + 0.5F - lens.project.cy) * BLOB_DEPTH / lens.focal_y, BLOB_DEPTH};
    r3d_span_target_t target = r3d_span_target(color, depth, BLOB_WIDTH, 0, BLOB_HEIGHT);
    contact_blob_draw(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 0);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    depth[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE] = R3D_DEPTH_NEAREST;
    contact_blob_draw(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    depth[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE] = R3D_DEPTH_EMPTY;
    for (int i = 0; i < BLOB_WIDTH * BLOB_HEIGHT; i++) {
        color[i] = RGB565BE(0xffffff);
    }
    target = r3d_span_target(color + BLOB_CENTRE * BLOB_WIDTH, depth + BLOB_CENTRE * BLOB_WIDTH, BLOB_WIDTH,
                             BLOB_CENTRE, BLOB_CENTRE + 1);
    contact_blob_draw(&target, &lens, ground, vec3f_unit_z(), BLOB_RADIUS, 1);
    TEST_ASSERT_EQUAL_HEX16(rgb565be_pack(15, 31, 15), color[BLOB_CENTRE * BLOB_WIDTH + BLOB_CENTRE]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[0]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[(BLOB_CENTRE - 1) * BLOB_WIDTH + BLOB_CENTRE]);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xffffff), color[BLOB_CENTRE * BLOB_WIDTH]);
    free(depth);
    free(color);
}

void
suite_contact_blob(void) {
    RUN_TEST(test_blob_depth_radius_rows_strength);
}

SUITE_REGISTER(suite_contact_blob);
