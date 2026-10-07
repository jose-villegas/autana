/*
 * Portable suite: raster_attachment.h. Further attachments carved beside
 * colour and depth, their writers following the fill, and their begin and
 * resolve hooks around a picture. Every mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "r3d_quad_mesh.h"
#include "suites.h"
#include "unity.h"

#include "render/r3d.h"
#include "render/r3d_pipeline.h"

#define W 64
#define H 48

/* Two quads facing +z: a wall 800 square on z = 0 and a card 80 square in
 * front of it, each a mesh drawn as its own instance. */
static const int16_t wall_positions[][3] = {{-400, -400, 0}, {400, -400, 0}, {400, 400, 0}, {-400, 400, 0}};
static const int16_t card_positions[][3] = {{-40, -40, 100}, {40, -40, 100}, {40, 40, 100}, {-40, 40, 100}};
static const r3d_lit_node_t wall_node = {{-400, -400, 0}, {400, 400, 0}, 0, 1, true};
static const r3d_lit_node_t card_node = {{-40, -40, 100}, {40, 40, 100}, 0, 1, true};
static const r3d_lit_cluster_t wall_cluster = {0, 4, 0, 2, {-400, -400, 0}, {400, 400, 0}, true};
static const r3d_lit_cluster_t card_cluster = {0, 4, 0, 2, {-40, -40, 100}, {40, 40, 100}, true};

/* An attachment that writes, where a triangle won, its instance plus one
 * times its own `state` factor, so two of them write different values. */
static void
id_clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

static void
id_span(const r3d_span_writer_t* writer, const gfx_render_target_t* rows, int y, int x_first, int x_last, int32_t z,
        int32_t dz) {
    const uint16_t* depth = gfx_render_target_depth(rows, y);
    uint16_t* id = gfx_render_target_row(rows, writer->attachment, y);
    for (int x = x_first; x <= x_last; x++, z += dz) {
        id[x] = (uint16_t)(z >> 8) == depth[x] ? (uint16_t)writer->value : id[x];
    }
}

static bool
id_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    out->span = id_span;
    out->value = (uint32_t)(instance + 1) * *(const uint32_t*)self->state;
    return true;
}

/* An attachment counting its begin calls and marking every row resolve
 * passes, once each. */
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

typedef struct {
    r3d_lit_mesh_t wall, card;
    r3d_instance_t instances[2];
    raster_t raster;
} rig_t;

static rig_t* rig;

static void
release_rig(void) {
    if (rig != NULL) {
        free(rig->raster.scratch);
        free(rig);
        rig = NULL;
    }
}

/* Allocated per test: the wall then the card, `attachments` attached, scratch
 * for the largest size a test draws, 2W x 2H. */
static rig_t*
rig_open(const raster_attachment_t* const* attachments, int count) {
    rig = calloc(1, sizeof(*rig));
    TEST_ASSERT_NOT_NULL(rig);
    suite_set_test_cleanup(release_rig);
    rig->wall = r3d_quad_mesh(wall_positions, NULL, NULL, &wall_cluster, &wall_node);
    rig->card = r3d_quad_mesh(card_positions, NULL, NULL, &card_cluster, &card_node);
    rig->instances[0] = (r3d_instance_t){&rig->wall, NULL};
    rig->instances[1] = (r3d_instance_t){&rig->card, NULL};
    rig->raster = (raster_t){.instances = rig->instances,
                             .instance_count = 2,
                             .width = 2 * W,
                             .height = 2 * H,
                             .clear = 0x1234,
                             .attachments = attachments,
                             .attachment_count = count};
    rig->raster.scratch = malloc(raster_scratch_bytes(&rig->raster));
    TEST_ASSERT_NOT_NULL(rig->raster.scratch);
    rig->raster.width = W;
    rig->raster.height = H;
    return rig;
}

static void
draw(rig_t* r) {
    const camera_t camera = {{0.0F, 0.0F, 300.0F}, {0.0F, 0.0F, -1.0F}, 0.5F, 1.0F};
    raster_draw(&r->raster, &camera, 0);
}

static const uint16_t*
attachment(const rig_t* r, int index) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(&r->raster);
    return b.picture.attachment[index].pixels;
}

static void
test_two_writing_attachments_each_record_the_instance_that_won(void) {
    static uint32_t ones = 1;
    static uint32_t tens = 10;
    const raster_attachment_t a = {sizeof(uint16_t), id_clear, NULL, id_writer, NULL, NULL, &ones};
    const raster_attachment_t b = {sizeof(uint16_t), id_clear, NULL, id_writer, NULL, NULL, &tens};
    const raster_attachment_t* const both[] = {&a, &b};
    rig_t* r = rig_open(both, 2);
    draw(r);
    const int centre = (H / 2 * W) + (W / 2);
    const int corner = (2 * W) + 2;
    TEST_ASSERT_EQUAL_UINT16(2, attachment(r, GFX_ATTACHMENT_FURTHER)[centre]); /* the card, in front */
    TEST_ASSERT_EQUAL_UINT16(1, attachment(r, GFX_ATTACHMENT_FURTHER)[corner]); /* the wall alone */
    TEST_ASSERT_EQUAL_UINT16(20, attachment(r, GFX_ATTACHMENT_FURTHER + 1)[centre]);
    TEST_ASSERT_EQUAL_UINT16(10, attachment(r, GFX_ATTACHMENT_FURTHER + 1)[corner]);
}

static void
test_attachments_leave_colour_and_depth_as_they_are(void) {
    uint16_t* plain = malloc(sizeof(uint16_t) * 2 * W * H);
    TEST_ASSERT_NOT_NULL(plain);
    rig_t* r = rig_open(NULL, 0);
    draw(r);
    memcpy(plain, raster_color(&r->raster), sizeof(uint16_t) * W * H);
    memcpy(plain + (W * H), raster_depth(&r->raster), sizeof(uint16_t) * W * H);
    release_rig();
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
    static uint32_t ones = 1;
    const raster_attachment_t wide = {4, id_clear, NULL, NULL, NULL, NULL, &ones};
    const raster_attachment_t* const one[] = {&wide};
    rig_t* r = rig_open(one, 1);
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
    rig_t* r = rig_open(one, 1);
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
