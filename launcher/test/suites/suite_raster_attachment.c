/*
 * Portable suite: raster_attachment.h. Further attachments carved beside
 * colour and depth, their writers following the fill, and their begin and
 * resolve hooks around a picture. Every mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "suites.h"
#include "unity.h"

#define W 64
#define H 48

/* The rig's wall, then a card 80 square in front of it, also facing +z. */
static const int16_t card[][3] = {{-40, -40, 100}, {40, -40, 100}, {40, 40, 100}, {-40, 40, 100}};
static const int16_t (*const wall_and_card[])[3] = {raster_rig_wall, card};

static void
id_clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

/* Tags each pixel with the instance that won it, plus one, times its own
 * `state` factor, so two of them write different values. */
static bool
id_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    out->span = raster_attachment_tag;
    out->value = (uint32_t)(instance + 1) * *(const uint32_t*)self->state;
    return true;
}

/* Counts its begin calls and marks every row resolve passes. */
typedef struct {
    int begins;
    uint8_t resolved[H];
} hooks_t;

static void
hooks_begin(const raster_attachment_t* self, const raster_t* raster, const camera_t* camera, int quarter) {
    (void)raster;
    (void)camera;
    (void)quarter;
    ((hooks_t*)self->state)->begins++;
}

static void
hooks_resolve(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* rows, int index) {
    (void)raster;
    (void)index;
    for (int y = rows->row0; y < rows->row1; y++) {
        ((hooks_t*)self->state)->resolved[y]++;
    }
}

static raster_rig_t*
rig_open(const raster_attachment_t* const* attachments, int count) {
    raster_rig_t* r = raster_rig_open(wall_and_card, 2, 0, W, H, 0);
    raster_rig_attach(r, attachments, count);
    return r;
}

static void
draw(raster_rig_t* r) {
    const camera_t camera = {{0.0F, 0.0F, 300.0F}, {0.0F, 0.0F, -1.0F}, 0.5F, 1.0F};
    raster_draw(&r->raster, &camera, 0);
}

static void
test_two_writing_attachments_each_record_the_instance_that_won(void) {
    static uint32_t ones = 1;
    static uint32_t tens = 10;
    const raster_attachment_t a = {sizeof(uint16_t), id_clear, NULL, id_writer, NULL, NULL, &ones};
    const raster_attachment_t b = {sizeof(uint16_t), id_clear, NULL, id_writer, NULL, NULL, &tens};
    const raster_attachment_t* const both[] = {&a, &b};
    raster_rig_t* r = rig_open(both, 2);
    draw(r);
    const uint16_t* first = raster_rig_attachment(r, 0);
    const uint16_t* second = raster_rig_attachment(r, 1);
    const int centre = (H / 2 * W) + (W / 2);
    const int corner = (2 * W) + 2;
    TEST_ASSERT_EQUAL_UINT16(2, first[centre]); /* the card, in front */
    TEST_ASSERT_EQUAL_UINT16(1, first[corner]); /* the wall alone */
    TEST_ASSERT_EQUAL_UINT16(20, second[centre]);
    TEST_ASSERT_EQUAL_UINT16(10, second[corner]);
}

static void
test_attachments_leave_colour_and_depth_as_they_are(void) {
    uint16_t* plain = malloc(sizeof(uint16_t) * 2 * W * H);
    TEST_ASSERT_NOT_NULL(plain);
    raster_rig_t* r = rig_open(NULL, 0);
    draw(r);
    memcpy(plain, raster_color(&r->raster), sizeof(uint16_t) * W * H);
    memcpy(plain + (W * H), raster_depth(&r->raster), sizeof(uint16_t) * W * H);
    raster_rig_release();
    static uint32_t ones = 1;
    const raster_attachment_t a = {sizeof(uint16_t), id_clear, NULL, id_writer, NULL, NULL, &ones};
    const raster_attachment_t* const one[] = {&a};
    r = rig_open(one, 1);
    draw(r);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(plain, raster_color(&r->raster), W * H);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(plain + (W * H), raster_depth(&r->raster), W * H);
    free(plain);
}

/* Each attachment takes its bytes per pixel times the drawn size, rounded
 * to 4, and a new size carves every one anew. */
static void
test_every_attachment_is_carved_at_the_drawn_size(void) {
    const raster_attachment_t wide = {4, id_clear, NULL, NULL, NULL, NULL, NULL};
    const raster_attachment_t* const one[] = {&wide};
    raster_rig_t* r = raster_rig_open(wall_and_card, 2, 0, 2 * W, 2 * H, 0);
    raster_rig_attach(r, one, 1);
    r->raster.width = W;
    r->raster.height = H;
    raster_t plain = r->raster;
    plain.attachment_count = 0;
    TEST_ASSERT_EQUAL_size_t(raster_scratch_bytes(&plain) + (4U * W * H), raster_scratch_bytes(&r->raster));
    const gfx_render_target_t small = r3d_pipeline_carve(&r->raster).picture;
    r->raster.width = 2 * W;
    r->raster.height = 2 * H;
    const gfx_render_target_t large = r3d_pipeline_carve(&r->raster).picture;
    TEST_ASSERT_EQUAL_INT(2 * W, large.width);
    TEST_ASSERT_EQUAL_INT(2 * H, large.row1);
    TEST_ASSERT_EQUAL_PTR((char*)small.attachment[1].pixels + (2U * W * H), small.attachment[2].pixels);
    TEST_ASSERT_EQUAL_PTR((char*)large.attachment[1].pixels + (8U * W * H), large.attachment[2].pixels);
}

static void
test_begin_runs_once_and_resolve_covers_every_row_once(void) {
    hooks_t* hooks = calloc(1, sizeof(*hooks));
    TEST_ASSERT_NOT_NULL(hooks);
    const raster_attachment_t a = {sizeof(uint16_t), id_clear, hooks_begin, NULL, hooks_resolve, NULL, hooks};
    const raster_attachment_t* const one[] = {&a};
    raster_rig_t* r = rig_open(one, 1);
    draw(r);
    TEST_ASSERT_EQUAL_INT(1, hooks->begins);
    for (int y = 0; y < H; y++) {
        TEST_ASSERT_EQUAL_UINT8(1, hooks->resolved[y]);
    }
    free(hooks);
}

void
run_raster_attachment_suite(void) {
    RUN_TEST(test_two_writing_attachments_each_record_the_instance_that_won);
    RUN_TEST(test_attachments_leave_colour_and_depth_as_they_are);
    RUN_TEST(test_every_attachment_is_carved_at_the_drawn_size);
    RUN_TEST(test_begin_runs_once_and_resolve_covers_every_row_once);
}

SUITE_REGISTER(run_raster_attachment_suite);
