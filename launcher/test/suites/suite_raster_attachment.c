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
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "render/context/render_context.h"
#include "render/raster_meshlets.h"
#include "render/raster_motion.h"
#include "render/raster_show.h"
#include "util/runtime/memory.h"
#ifdef HOST_HEAP_ARENA
#include "heap_arena.h"
#endif

#define W 64
#define H 48

/* The rig's wall, then a card 80 square in front of it, also facing +z. */
static const int16_t card[][3] = {{-40, -40, 100}, {40, -40, 100}, {40, 40, 100}, {-40, 40, 100}};
static const int16_t (*const wall_and_card[])[3] = {raster_rig_wall, card};
static const fixture_camera_t camera = {{0.0F, 0.0F, 300.0F}, {0.0F, 0.0F, -1.0F}, 0.5F, 1.0F};

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

static bool
cluster_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    id_writer(self, instance, out);
    out->per_cluster = true;
    return true;
}

/* Counts its begin calls and marks every row resolve passes. */
typedef struct {
    int begins;
    uint8_t resolved[H];
} hooks_t;

static void
hooks_begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    (void)raster;
    (void)view;
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
    raster_rig_t* r = raster_rig_open(wall_and_card, 2, 0, W, H, sizeof(int16_t) * 8 * 3);
    raster_rig_attach(r, attachments, count);
    return r;
}

static void
draw(raster_rig_t* r) {
    const render_view_t frame_view = render_view_fixture(&camera, &r->raster, 0);
    raster_draw(&r->raster, &frame_view);
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

static void
split_quad(raster_rig_t* rig, r3d_lit_cluster_t clusters[2]) {
    static const uint16_t white[] = {UINT16_MAX, UINT16_MAX};
    r3d_quad_t* quad = &rig->quad[0];
    int16_t(*positions)[3] = (void*)rig->own;
    memcpy(positions, quad->mesh.positions, sizeof(int16_t) * 4 * 3);
    memcpy(positions + 4, quad->mesh.positions, sizeof(int16_t) * 4 * 3);
    clusters[0] = clusters[1] = quad->cluster;
    clusters[0].triangle_count = clusters[1].triangle_count = 1;
    clusters[1].triangle_first = 1;
    clusters[1].vertex_first = 4;
    quad->mesh.positions = (const int16_t(*)[3])positions;
    quad->mesh.vertex_count = 8;
    quad->mesh.colors = NULL;
    quad->mesh.face_colors = white;
    quad->mesh.clusters = clusters;
    quad->mesh.cluster_count = 2;
    quad->node.count = 2;
}

static void
test_cluster_values_and_constant_writers(void) {
    uint32_t base = 17;
    const raster_attachment_t per = {sizeof(uint16_t), id_clear, NULL, cluster_writer, NULL, NULL, &base};
    const raster_attachment_t constant = {sizeof(uint16_t), id_clear, NULL, id_writer, NULL, NULL, &base};
    const raster_attachment_t* both[] = {&per, &constant};
    raster_rig_t* r = rig_open(both, 2);
    r->raster.instance_count = 1;
    r3d_lit_cluster_t clusters[2];
    split_quad(r, clusters);
    raster_rig_attach(r, both, 2);
    draw(r);
    const uint16_t* ids = raster_rig_attachment(r, 0);
    const uint16_t* fixed = raster_rig_attachment(r, 1);
    int seen[2] = {0};
    for (int p = 0; p < W * H; p++) {
        TEST_ASSERT_EQUAL_UINT16(17, fixed[p]);
        TEST_ASSERT_TRUE(ids[p] == 17 || ids[p] == 18);
        seen[ids[p] - 17]++;
    }
    TEST_ASSERT_TRUE(seen[0] > 0 && seen[1] > 0);
    TEST_ASSERT_EQUAL_UINT32(17, base);

    const r3d_pipeline_buffers_t buffers = r3d_pipeline_carve(&r->raster);
    memset(raster_depth(&r->raster), 0, sizeof(uint16_t) * W * H);
    for (int k = 0; k < 2; k++) {
        id_clear(both[k], &r->raster, buffers.picture.attachment[GFX_ATTACHMENT_FURTHER + k].pixels, W * H);
    }
    r3d_span_writer_t writers[2] = {0};
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_TRUE(both[k]->writer(both[k], 0, &writers[k]));
        writers[k].attachment = GFX_ATTACHMENT_FURTHER + k;
    }
    const r3d_span_target_t target = {buffers.picture, writers, 2};
    r->view = render_view_fixture(&camera, &r->raster, 0);
    raster_lens(&r->raster, &r->view, 1, &r->lens);
    const uint16_t visible = 1;
    r3d_pipeline_transform(&r->quad[0].mesh, &r->lens, &visible, 1, buffers.cs, buffers.rows);
    r3d_pipeline_draw(&r->quad[0].mesh, &r->lens, &visible, 1, buffers.cs, buffers.rows, &target, buffers.work[0]);
    int drawn = 0;
    for (int p = 0; p < W * H; p++) {
        const bool won = raster_depth(&r->raster)[p] != 0;
        TEST_ASSERT_EQUAL_UINT16(won ? 18 : 0, ids[p]);
        TEST_ASSERT_EQUAL_UINT16(won ? 17 : 0, fixed[p]);
        drawn += won;
    }
    TEST_ASSERT_TRUE(drawn > 0);
    TEST_ASSERT_EQUAL_UINT32(17, writers[0].value);
    TEST_ASSERT_EQUAL_UINT32(17, writers[1].value);
}

static void
test_meshlet_ranges_restart_each_draw_and_show_empty_as_clear(void) {
    raster_meshlets_t state = {0};
    const raster_attachment_t view = raster_meshlets_view(&state);
    const raster_attachment_t* one[] = {&view};
    raster_rig_t* r = rig_open(one, 1);
    r3d_lit_cluster_t clusters[2];
    split_quad(r, clusters);
    raster_rig_attach(r, one, 1);
    for (int frame = 0; frame < 2; frame++) {
        draw(r);
        const uint16_t* ids = raster_rig_attachment(r, 0);
        int seen[4] = {0};
        for (int p = 0; p < W * H; p++) {
            TEST_ASSERT_TRUE(ids[p] >= 1 && ids[p] <= 3);
            seen[ids[p]]++;
        }
        TEST_ASSERT_TRUE(seen[1] > 0 && seen[2] > 0 && seen[3] > 0);
        TEST_ASSERT_EQUAL_UINT32(4, state.next);
        raster_show(&r->raster);
        const uint16_t* colors = raster_color(&r->raster);
        uint16_t palette[4] = {0};
        for (int p = 0; p < W * H; p++) {
            if (palette[ids[p]] != 0) {
                TEST_ASSERT_EQUAL_UINT16(palette[ids[p]], colors[p]);
            }
            palette[ids[p]] = colors[p];
        }
        TEST_ASSERT_TRUE(palette[1] != palette[2] && palette[2] != palette[3] && palette[1] != palette[3]);
    }
    r->raster.instance_count = 1;
    r->raster.instances = &r->instance[1];
    draw(r);
    const uint16_t* ids = raster_rig_attachment(r, 0);
    TEST_ASSERT_EQUAL_UINT16(0, ids[0]);
    raster_show(&r->raster);
    TEST_ASSERT_EQUAL_UINT16(r->raster.clear, raster_color(&r->raster)[0]);
}

static void
test_depth_and_tiles_rows_use_no_scratch(void) {
    raster_rig_t* r = rig_open(NULL, 0);
    const size_t plain = raster_scratch_bytes(&r->raster);
    for (int i = 0; i < 2; i++) {
        const raster_attachment_t view = render_context_view(i + 1)->attachment(NULL);
        const raster_attachment_t* one[] = {&view};
        raster_rig_attach(r, one, 1);
        TEST_ASSERT_EQUAL_size_t(plain, raster_scratch_bytes(&r->raster));
        draw(r);
    }
}

static bool
partial_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    if (instance == 0) {
        out->per_cluster = true;
        return false;
    }
    TEST_ASSERT_FALSE(out->per_cluster);
    return id_writer(self, instance, out);
}

static void
test_writer_slot_is_zeroed_before_each_hook(void) {
    uint32_t base = 1;
    const raster_attachment_t view = {sizeof(uint16_t), id_clear, NULL, partial_writer, NULL, NULL, &base};
    const raster_attachment_t* one[] = {&view};
    draw(rig_open(one, 1));
}

static void
test_same_view_survives_frames_and_contexts_own_their_states(void) {
    raster_rig_t* r = rig_open(NULL, 0);
    render_context_t* contexts = calloc(2, sizeof(*contexts));
    TEST_ASSERT_NOT_NULL(contexts);
    render_context_t* a = &contexts[0];
    render_context_t* b = &contexts[1];
    render_context_set_scale(a, 100);
    render_context_set_scale(b, 100);
    const int view = render_context_view_named("motion");
    render_context_set_view(a, view);
    render_context_set_view(b, view);
    raster_motion_t* state = a->view_state;
    TEST_ASSERT_TRUE(a->view_state != b->view_state);
    for (int frame = 0; frame < 3; frame++) {
#ifdef HOST_HEAP_ARENA
        size_t blocks, bytes, after_blocks, after_bytes;
        heap_arena_snapshot(&blocks, &bytes);
#endif
        render_context_set_view(a, view);
        TEST_ASSERT_EQUAL_PTR(state, a->view_state);
        TEST_ASSERT_EQUAL_INT(frame > 0, state->has_previous);
#ifdef HOST_HEAP_ARENA
        heap_arena_snapshot(&after_blocks, &after_bytes);
        TEST_ASSERT_EQUAL_size_t(blocks, after_blocks);
        TEST_ASSERT_EQUAL_size_t(bytes, after_bytes);
#endif
        const render_view_t frame_view = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
        TEST_ASSERT_TRUE(render_context_draw(a, r->instance, 2, &frame_view, 0));
        TEST_ASSERT_TRUE(state->has_previous);
        TEST_ASSERT_FALSE(((raster_motion_t*)b->view_state)->has_previous);
    }
    render_context_release(a);
    TEST_ASSERT_NOT_NULL(b->view_state);
    render_context_release(b);
    free(contexts);
}

static void
test_zero_context_draws_shaded_and_meshlet_context_paints_colour(void) {
    raster_rig_t* r = rig_open(NULL, 0);
    render_context_t c = {0};
    TEST_ASSERT_EQUAL_INT(RENDER_VIEW_SHADED, c.view);
    render_context_set_view(&c, RENDER_VIEW_SHADED);
    TEST_ASSERT_EQUAL_INT(0, c.view);
    render_context_set_scale(&c, 100);
    const render_view_t frame_view = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(&c, r->instance, 2, &frame_view, 0));
    TEST_ASSERT_EQUAL_INT(0, c.raster.attachment_count);
    uint16_t* shaded = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(shaded);
    memcpy(shaded, raster_color(&c.raster), sizeof(uint16_t) * W * H);
    render_context_set_view(&c, render_context_view_named("meshlets"));
    TEST_ASSERT_TRUE(render_context_draw(&c, r->instance, 2, &frame_view, 0));
    TEST_ASSERT_TRUE(memcmp(shaded, raster_color(&c.raster), sizeof(uint16_t) * W * H) != 0);
    free(shaded);
    render_context_release(&c);
}

#ifdef HOST_HEAP_ARENA
static void
test_failed_view_allocation_leaves_shaded(void) {
    render_context_t c = {0};
    render_context_set_view(&c, render_context_view_named("depth"));
    void* occupied = memory_alloc(memory_largest_block(MEMORY_PSRAM), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(occupied);
    render_context_set_view(&c, render_context_view_named("motion"));
    memory_free(occupied);
    TEST_ASSERT_EQUAL_INT(0, c.view);
    TEST_ASSERT_NULL(c.view_state);
    TEST_ASSERT_NULL(c.raster.attachments);
    TEST_ASSERT_EQUAL_INT(0, c.raster.attachment_count);
    render_context_release(&c);
}
#endif

static void
test_view_table_and_context_ownership(void) {
    render_context_t c = {0};
#ifdef HOST_HEAP_ARENA
    size_t before_blocks, before_bytes, blocks, bytes;
    heap_arena_snapshot(&before_blocks, &before_bytes);
#endif
    static const char* const names[] = {"depth", "tiles", "motion", "meshlets"};
    TEST_ASSERT_EQUAL_INT(sizeof names / sizeof names[0], RENDER_VIEW_COUNT);
    TEST_ASSERT_EQUAL_INT(RENDER_VIEW_SHADED, render_context_view_named("shaded"));
    TEST_ASSERT_EQUAL_INT(RENDER_VIEW_UNKNOWN, render_context_view_named("unknown"));
    TEST_ASSERT_NULL(render_context_view(RENDER_VIEW_SHADED));
    TEST_ASSERT_NULL(render_context_view(RENDER_VIEW_COUNT + 1));
    for (int i = 1; i <= RENDER_VIEW_COUNT; i++) {
        const render_debug_view_t* row = render_context_view(i);
        TEST_ASSERT_NOT_NULL(row);
        TEST_ASSERT_EQUAL_STRING(names[i - 1], row->name);
        TEST_ASSERT_EQUAL_INT(i, render_context_view_named(row->name));
        for (int j = 1; j < i; j++) {
            TEST_ASSERT_TRUE(strcmp(row->name, render_context_view(j)->name) != 0);
        }
        render_context_set_view(&c, i);
        TEST_ASSERT_EQUAL_INT(i, c.view);
        TEST_ASSERT_EQUAL_INT(1, c.raster.attachment_count);
        TEST_ASSERT_EQUAL_PTR(&c.view_attachment, c.raster.attachments[0]);
        const raster_attachment_t expected = row->attachment(c.view_state);
        TEST_ASSERT_TRUE(c.view_attachment.show == expected.show);
        TEST_ASSERT_EQUAL_INT(expected.bytes_per_pixel, c.view_attachment.bytes_per_pixel);
        TEST_ASSERT_EQUAL_PTR(c.view_state, c.view_attachment.state);
        if (row->state_bytes > 0) {
            TEST_ASSERT_NOT_NULL(c.view_state);
            for (size_t k = 0; k < row->state_bytes; k++) {
                TEST_ASSERT_EQUAL_UINT8(0, ((const uint8_t*)c.view_state)[k]);
            }
        }
#ifdef HOST_HEAP_ARENA
        heap_arena_snapshot(&blocks, &bytes);
        TEST_ASSERT_EQUAL_size_t(before_blocks + (row->state_bytes > 0), blocks);
#endif
    }
    render_context_set_view(&c, RENDER_VIEW_SHADED);
    TEST_ASSERT_NULL(c.view_state);
    TEST_ASSERT_EQUAL_INT(0, c.raster.attachment_count);
#ifdef HOST_HEAP_ARENA
    heap_arena_snapshot(&blocks, &bytes);
    TEST_ASSERT_EQUAL_size_t(before_blocks, blocks);
    TEST_ASSERT_EQUAL_size_t(before_bytes, bytes);
#endif
    render_context_set_view(&c, RENDER_VIEW_COUNT);
    render_context_release(&c);
    TEST_ASSERT_NULL(c.view_state);
    TEST_ASSERT_EQUAL_INT(0, c.view);
#ifdef HOST_HEAP_ARENA
    heap_arena_snapshot(&blocks, &bytes);
    TEST_ASSERT_EQUAL_size_t(before_blocks, blocks);
    TEST_ASSERT_EQUAL_size_t(before_bytes, bytes);
#endif
}

#ifndef DEVICE_BUILD
static void
test_printed_views_match_the_declared_names(void) {
    FILE* out = tmpfile();
    TEST_ASSERT_NOT_NULL(out);
    render_context_print_views(out);
    rewind(out);
    char names[128] = {0};
    const size_t bytes = fread(names, 1, sizeof names - 1, out);
    fclose(out);
    TEST_ASSERT_EQUAL_STRING("shaded, depth, tiles, motion, meshlets", names);
    int count = 1;
    for (size_t i = 0; i < bytes; i++) {
        count += names[i] == ',';
    }
    TEST_ASSERT_EQUAL_INT(RENDER_VIEW_COUNT + 1, count);
}
#endif

static void
test_meshlet_writer_accepts_the_last_uint16_id(void) {
    const r3d_lit_mesh_t meshes[] = {{.cluster_count = UINT16_MAX - 1}, {.cluster_count = 1}};
    const r3d_instance_t instances[] = {{.mesh = &meshes[0]}, {.mesh = &meshes[1]}};
    const raster_t raster = {.instances = instances, .instance_count = 2};
    raster_meshlets_t state = {0};
    const raster_attachment_t view = raster_meshlets_view(&state);
    const render_view_t frame_view = render_view_fixture_at(&camera, (viewport_t){W, H, 0});
    view.begin(&view, &raster, &frame_view);
    r3d_span_writer_t out = {0};
    TEST_ASSERT_TRUE(view.writer(&view, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(1, out.value);
    TEST_ASSERT_TRUE(view.writer(&view, 1, &out));
    TEST_ASSERT_EQUAL_UINT32(UINT16_MAX, out.value);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)UINT16_MAX + 1, state.next);
}

void
run_raster_attachment_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_printed_views_match_the_declared_names);
#endif
    RUN_TEST(test_meshlet_writer_accepts_the_last_uint16_id);
#ifdef HOST_HEAP_ARENA
    RUN_TEST(test_failed_view_allocation_leaves_shaded);
#endif
    RUN_TEST(test_cluster_values_and_constant_writers);
    RUN_TEST(test_meshlet_ranges_restart_each_draw_and_show_empty_as_clear);
    RUN_TEST(test_depth_and_tiles_rows_use_no_scratch);
    RUN_TEST(test_writer_slot_is_zeroed_before_each_hook);
    RUN_TEST(test_same_view_survives_frames_and_contexts_own_their_states);
    RUN_TEST(test_zero_context_draws_shaded_and_meshlet_context_paints_colour);
    RUN_TEST(test_view_table_and_context_ownership);
    RUN_TEST(test_two_writing_attachments_each_record_the_instance_that_won);
    RUN_TEST(test_attachments_leave_colour_and_depth_as_they_are);
    RUN_TEST(test_every_attachment_is_carved_at_the_drawn_size);
    RUN_TEST(test_begin_runs_once_and_resolve_covers_every_row_once);
}

SUITE_REGISTER(run_raster_attachment_suite);
