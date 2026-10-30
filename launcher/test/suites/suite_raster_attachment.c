/*
 * Portable suite: raster_attachment.h. Further attachments carved beside
 * colour and depth, their writers following the fill, and their begin and
 * resolve hooks around a picture. Every mesh is built inside the test.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "math/scalar/mathf_round.h"
#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "core/memory.h"
#include "display/viewport.h"
#include "render/context/render_context.h"
#include "render/r3d_lit_mesh.h"
#include "render/raster.h"
#include "render/view/raster_meshlets.h"
#include "render/view/raster_motion.h"
#include "render/view/raster_show.h"
#ifdef HOST_HEAP_ARENA
#include "heap_arena.h"
#endif

#define W 64
#define H 48

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
    out->step = R3D_SPAN_STEP_CLUSTER;
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
hooks_resolve(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* rows, int index) {
    (void)raster;
    (void)index;
    for (int y = rows->first_row; y < gfx_target_end_row(rows); y++) {
        ((hooks_t*)self->state)->resolved[y]++;
    }
}

static raster_rig_t*
rig_open(const raster_attachment_t* const* attachments, int count) {
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, 2, 0, W, H, sizeof(int16_t) * 8 * 3);
    raster_rig_attach(r, attachments, count);
    return r;
}

static void
draw(raster_rig_t* r) {
    r->view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    raster_draw(&r->raster, &r->view);
}

static void
test_two_writing_attachments_each_record_the_instance_that_won(void) {
    static uint32_t ones = 1;
    static uint32_t tens = 10;
    const raster_attachment_t a = {.format = IMAGE_U16, .clear = id_clear, .writer = id_writer, .state = &ones};
    const raster_attachment_t b = {.format = IMAGE_U16, .clear = id_clear, .writer = id_writer, .state = &tens};
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
    const raster_attachment_t a = {.format = IMAGE_U16, .clear = id_clear, .writer = id_writer, .state = &ones};
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
    const raster_attachment_t wide = {.format = IMAGE_U32, .clear = id_clear};
    const raster_attachment_t* const one[] = {&wide};
    raster_rig_t* r = raster_rig_open(raster_rig_wall_and_card, 2, 0, 2 * W, 2 * H, 0);
    raster_rig_attach(r, one, 1);
    r->raster.width = W;
    r->raster.height = H;
    raster_t plain = r->raster;
    plain.attachment_count = 0;
    TEST_ASSERT_EQUAL_size_t(raster_scratch_bytes(&plain) + (4U * W * H), raster_scratch_bytes(&r->raster));
    const raster_buffers_t small_buffers = raster_carve(&r->raster);
    const gfx_target_t* small = &small_buffers.picture;
    r->raster.width = 2 * W;
    r->raster.height = 2 * H;
    const raster_buffers_t large_buffers = raster_carve(&r->raster);
    const gfx_target_t* large = &large_buffers.picture;
    TEST_ASSERT_EQUAL_INT(2 * W, large->attachment[0].width);
    TEST_ASSERT_EQUAL_INT(2 * H, large->attachment[0].height);
    TEST_ASSERT_EQUAL_PTR((char*)small->attachment[1].pixels + (2U * W * H), small->attachment[2].pixels);
    TEST_ASSERT_EQUAL_PTR((char*)large->attachment[1].pixels + (8U * W * H), large->attachment[2].pixels);
}

static void
test_begin_runs_once_and_resolve_covers_every_row_once(void) {
    hooks_t* hooks = calloc(1, sizeof(*hooks));
    TEST_ASSERT_NOT_NULL(hooks);
    const raster_attachment_t a = {
        .format = IMAGE_U16, .clear = id_clear, .begin = hooks_begin, .resolve = hooks_resolve, .state = hooks};
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
    int16_t (*positions)[3] = (void*)rig->own;
    memcpy(positions, quad->mesh.positions, sizeof(int16_t) * 4 * 3);
    memcpy(positions + 4, quad->mesh.positions, sizeof(int16_t) * 4 * 3);
    clusters[0] = clusters[1] = quad->cluster;
    clusters[0].triangle_count = clusters[1].triangle_count = 1;
    clusters[1].triangle_first = 1;
    clusters[1].vertex_first = 4;
    quad->mesh.positions = (const int16_t (*)[3])positions;
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
    const raster_attachment_t per = {.format = IMAGE_U16, .clear = id_clear, .writer = cluster_writer, .state = &base};
    const raster_attachment_t constant = {.format = IMAGE_U16, .clear = id_clear, .writer = id_writer, .state = &base};
    const raster_attachment_t* both[] = {&per, &constant};
    raster_rig_t* r = rig_open(both, 2);
    r->raster.instance_count = 1;
    r3d_lit_cluster_t* clusters = calloc(2, sizeof(*clusters));
    TEST_ASSERT_NOT_NULL(clusters);
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

    const raster_buffers_t buffers = raster_carve(&r->raster);
    memset(raster_depth(&r->raster), 0, sizeof(uint16_t) * W * H);
    for (int k = 0; k < 2; k++) {
        id_clear(both[k], &r->raster, buffers.picture.attachment[GFX_ATTACHMENT_FURTHER + k].pixels, W * H);
    }
    r3d_span_writer_t writers[2] = {0};
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_TRUE(both[k]->writer(both[k], 0, &writers[k]));
        writers[k].attachment = GFX_ATTACHMENT_FURTHER + k;
    }
    r3d_span_target_t target = {.writers = writers, .writer_count = 2, .small_fill = &r3d_span_small_fill_default};
    gfx_target_window(&target.rows, &buffers.picture, 0, H);
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
    free(clusters);
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
        const raster_attachment_t view = render_context_debug_view(i + 1)->attachment(NULL);
        const raster_attachment_t* one[] = {&view};
        raster_rig_attach(r, one, 1);
        TEST_ASSERT_EQUAL_size_t(plain, raster_scratch_bytes(&r->raster));
        draw(r);
    }
}

static bool
partial_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    if (instance == 0) {
        out->step = R3D_SPAN_STEP_CLUSTER;
        return false;
    }
    TEST_ASSERT_EQUAL_INT(R3D_SPAN_STEP_NONE, out->step);
    return id_writer(self, instance, out);
}

static void
test_writer_slot_is_zeroed_before_each_hook(void) {
    uint32_t base = 1;
    const raster_attachment_t view = {.format = IMAGE_U16, .clear = id_clear, .writer = partial_writer, .state = &base};
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
    const int view = render_context_debug_view_named("motion");
    render_context_set_debug_view(a, view);
    render_context_set_debug_view(b, view);
    raster_motion_t* state = a->debug_view_state;
    TEST_ASSERT_TRUE(a->debug_view_state != b->debug_view_state);
    for (int frame = 0; frame < 3; frame++) {
#ifdef HOST_HEAP_ARENA
        size_t blocks, bytes, after_blocks, after_bytes;
        heap_arena_snapshot(&blocks, &bytes);
#endif
        render_context_set_debug_view(a, view);
        TEST_ASSERT_EQUAL_PTR(state, a->debug_view_state);
        TEST_ASSERT_EQUAL_INT(frame > 0, state->has_previous);
#ifdef HOST_HEAP_ARENA
        heap_arena_snapshot(&after_blocks, &after_bytes);
        TEST_ASSERT_EQUAL_size_t(blocks, after_blocks);
        TEST_ASSERT_EQUAL_size_t(bytes, after_bytes);
#endif
        const render_view_t frame_view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
        TEST_ASSERT_TRUE(render_context_draw(a, r->instance, 2, &frame_view, 0));
        TEST_ASSERT_TRUE(state->has_previous);
        TEST_ASSERT_FALSE(((raster_motion_t*)b->debug_view_state)->has_previous);
    }
    render_context_release(a);
    TEST_ASSERT_NOT_NULL(b->debug_view_state);
    render_context_release(b);
    free(contexts);
}

static void
test_zero_context_draws_shaded_and_meshlet_context_paints_colour(void) {
    raster_rig_t* r = rig_open(NULL, 0);
    render_context_t c = {0};
    TEST_ASSERT_EQUAL_INT(RENDER_DEBUG_VIEW_SHADED, c.debug_view);
    render_context_set_debug_view(&c, RENDER_DEBUG_VIEW_SHADED);
    TEST_ASSERT_EQUAL_INT(0, c.debug_view);
    render_context_set_scale(&c, 100);
    const render_view_t frame_view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(&c, r->instance, 2, &frame_view, 0));
    TEST_ASSERT_EQUAL_INT(0, c.raster.attachment_count);
    uint16_t* shaded = malloc(sizeof(uint16_t) * W * H);
    TEST_ASSERT_NOT_NULL(shaded);
    memcpy(shaded, raster_color(&c.raster), sizeof(uint16_t) * W * H);
    render_context_set_debug_view(&c, render_context_debug_view_named("meshlets"));
    TEST_ASSERT_TRUE(render_context_draw(&c, r->instance, 2, &frame_view, 0));
    TEST_ASSERT_TRUE(memcmp(shaded, raster_color(&c.raster), sizeof(uint16_t) * W * H) != 0);
    free(shaded);
    render_context_release(&c);
}

#ifdef HOST_HEAP_ARENA
static void
test_failed_view_allocation_leaves_shaded(void) {
    render_context_t c = {0};
    render_context_set_debug_view(&c, render_context_debug_view_named("depth"));
    void* occupied = memory_alloc(memory_largest_block(MEMORY_PSRAM), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(occupied);
    render_context_set_debug_view(&c, render_context_debug_view_named("motion"));
    memory_free(occupied);
    TEST_ASSERT_EQUAL_INT(0, c.debug_view);
    TEST_ASSERT_NULL(c.debug_view_state);
    TEST_ASSERT_NULL(c.raster.attachments);
    TEST_ASSERT_EQUAL_INT(0, c.raster.attachment_count);
    render_context_release(&c);
}
#endif

static void
test_view_table_and_context_ownership(void) {
    render_context_t* c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
#ifdef HOST_HEAP_ARENA
    size_t before_blocks, before_bytes, blocks, bytes;
    heap_arena_snapshot(&before_blocks, &before_bytes);
#endif
    static const char* const names[] = {"depth", "tiles", "motion", "meshlets", "cull"};
    TEST_ASSERT_EQUAL_INT(sizeof names / sizeof names[0], RENDER_DEBUG_VIEW_COUNT);
    TEST_ASSERT_EQUAL_INT(RENDER_DEBUG_VIEW_SHADED, render_context_debug_view_named("shaded"));
    TEST_ASSERT_EQUAL_INT(RENDER_DEBUG_VIEW_UNKNOWN, render_context_debug_view_named("unknown"));
    TEST_ASSERT_NULL(render_context_debug_view(RENDER_DEBUG_VIEW_SHADED));
    TEST_ASSERT_NULL(render_context_debug_view(RENDER_DEBUG_VIEW_COUNT + 1));
    for (int i = 1; i <= RENDER_DEBUG_VIEW_COUNT; i++) {
        const render_debug_view_t* row = render_context_debug_view(i);
        TEST_ASSERT_NOT_NULL(row);
        TEST_ASSERT_EQUAL_STRING(names[i - 1], row->name);
        TEST_ASSERT_EQUAL_INT(i, render_context_debug_view_named(row->name));
        for (int j = 1; j < i; j++) {
            TEST_ASSERT_TRUE(strcmp(row->name, render_context_debug_view(j)->name) != 0);
        }
        render_context_set_debug_view(c, i);
        TEST_ASSERT_EQUAL_INT(i, c->debug_view);
        TEST_ASSERT_EQUAL_INT(1, c->raster.attachment_count);
        TEST_ASSERT_EQUAL_PTR(&c->debug_view_attachment, c->raster.attachments[0]);
        const raster_attachment_t expected = row->attachment(c->debug_view_state);
        TEST_ASSERT_TRUE(c->debug_view_attachment.show == expected.show);
        TEST_ASSERT_EQUAL_INT(expected.format, c->debug_view_attachment.format);
        TEST_ASSERT_EQUAL_PTR(c->debug_view_state, c->debug_view_attachment.state);
        if (row->state_bytes > 0) {
            TEST_ASSERT_NOT_NULL(c->debug_view_state);
            for (size_t k = 0; k < row->state_bytes; k++) {
                TEST_ASSERT_EQUAL_UINT8(0, ((const uint8_t*)c->debug_view_state)[k]);
            }
        }
#ifdef HOST_HEAP_ARENA
        heap_arena_snapshot(&blocks, &bytes);
        TEST_ASSERT_EQUAL_size_t(before_blocks + (row->state_bytes > 0), blocks);
#endif
    }
    render_context_set_debug_view(c, RENDER_DEBUG_VIEW_SHADED);
    TEST_ASSERT_NULL(c->debug_view_state);
    TEST_ASSERT_EQUAL_INT(0, c->raster.attachment_count);
#ifdef HOST_HEAP_ARENA
    heap_arena_snapshot(&blocks, &bytes);
    TEST_ASSERT_EQUAL_size_t(before_blocks, blocks);
    TEST_ASSERT_EQUAL_size_t(before_bytes, bytes);
#endif
    render_context_set_debug_view(c, RENDER_DEBUG_VIEW_COUNT);
    render_context_release(c);
    TEST_ASSERT_NULL(c->debug_view_state);
    TEST_ASSERT_EQUAL_INT(0, c->debug_view);
#ifdef HOST_HEAP_ARENA
    heap_arena_snapshot(&blocks, &bytes);
    TEST_ASSERT_EQUAL_size_t(before_blocks, blocks);
    TEST_ASSERT_EQUAL_size_t(before_bytes, bytes);
#endif
    free(c);
}

#ifndef DEVICE_BUILD
static void
test_printed_views_match_the_declared_names(void) {
    FILE* out = tmpfile();
    TEST_ASSERT_NOT_NULL(out);
    render_context_print_debug_views(out);
    rewind(out);
    char names[128] = {0};
    const size_t bytes = fread(names, 1, sizeof names - 1, out);
    fclose(out);
    TEST_ASSERT_EQUAL_STRING("shaded, depth, tiles, motion, meshlets, cull", names);
    int count = 1;
    for (size_t i = 0; i < bytes; i++) {
        count += names[i] == ',';
    }
    TEST_ASSERT_EQUAL_INT(RENDER_DEBUG_VIEW_COUNT + 1, count);
}
#endif

static void
test_meshlet_writer_accepts_the_last_uint16_id(void) {
    const r3d_lit_mesh_t meshes[] = {{.cluster_count = UINT16_MAX - 1}, {.cluster_count = 1}};
    const r3d_instance_t instances[] = {{.mesh = &meshes[0]}, {.mesh = &meshes[1]}};
    const raster_t raster = {.instances = instances, .instance_count = 2};
    raster_meshlets_t state = {0};
    const raster_attachment_t view = raster_meshlets_view(&state);
    const render_view_t frame_view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    view.begin(&view, &raster, &frame_view);
    r3d_span_writer_t out = {0};
    TEST_ASSERT_TRUE(view.writer(&view, 0, &out));
    TEST_ASSERT_EQUAL_UINT32(1, out.value);
    TEST_ASSERT_TRUE(view.writer(&view, 1, &out));
    TEST_ASSERT_EQUAL_UINT32(UINT16_MAX, out.value);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)UINT16_MAX + 1, state.next);
}

#define SMALL_FILL_CAMERA_DISTANCE 8

static raster_rig_t*
small_fill_fixture(bool at_gate) {
    static const int16_t corners[4][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    static const int16_t gate_corners[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const int16_t (*const positions[])[3] = {at_gate ? gate_corners : corners};
    static const uint8_t colors[4][3] = {{32, 80, 96}, {128, 112, 48}, {224, 160, 144}, {48, 96, 80}};
    const float tangent = at_gate ? H / (2.0F * SMALL_FILL_CAMERA_DISTANCE * (R3D_SPAN_SMALL_MAX_SIDE + 1)) : 1.0F;
    const fixture_camera_t close = {{0, 0, -SMALL_FILL_CAMERA_DISTANCE}, {0, 0, 1}, tangent, 1};
    raster_rig_t* r = raster_rig_open(positions, 1, 0, W, H, W * H * sizeof(uint16_t) * SMALL_FILL_STYLE_COUNT);
    r->quad[0].mesh.colors = colors;
    r->view = render_view_fixture(&close, &r->raster, 0);
    return r;
}

static __attribute__((noinline)) void
small_fill_reference(raster_rig_t* r, small_fill_t fill) {
    uint16_t* color = (uint16_t*)r->own;
    uint16_t* depth = color + W * H;
    for (int i = 0; i < W * H; i++) {
        color[i] = r->raster.clear;
        depth[i] = R3D_DEPTH_EMPTY;
    }
    r3d_span_small_fill_t resolved;
    r3d_span_small_fill_resolve(&resolved, fill);
    r3d_span_target_t target = r3d_span_target(color, depth, W, 0, H);
    target.small_fill = &resolved;
    r3d_span_vertex_t vertices[4];
    const float pixels_per_unit = r->view.lens.fx / SMALL_FILL_CAMERA_DISTANCE;
    for (int k = 0; k < 4; k++) {
        const int16_t* position = r->quad[0].mesh.positions[k];
        const uint8_t* rgb = r->quad[0].mesh.colors[k];
        vertices[k] = (r3d_span_vertex_t){mathf_round_i32((W / 2.0F + position[0] * pixels_per_unit) * R3D_SUBPIXEL),
                                          mathf_round_i32((H / 2.0F - position[1] * pixels_per_unit) * R3D_SUBPIXEL),
                                          1.0F / SMALL_FILL_CAMERA_DISTANCE,
                                          rgb[0],
                                          rgb[1],
                                          rgb[2]};
    }
    for (int triangle = 0; triangle < 2; triangle++) {
        const uint16_t* indices = r->quad[0].mesh.triangles[triangle];
        r3d_span_triangle(&target, vertices + indices[0], vertices + indices[1], vertices + indices[2]);
    }
}

static void
assert_small_fill_sequence(raster_rig_t* r, small_fill_t fill) {
    render_view_t a = r->view;
    a.small_fill = fill;
    small_fill_reference(r, fill);
    const size_t bytes = W * H * sizeof(uint16_t);
    uint16_t* expected = (uint16_t*)r->own + 2 * W * H;
    memcpy(expected, r->own, bytes);
    raster_draw(&r->raster, &a);
    TEST_ASSERT_EQUAL_MEMORY(expected, raster_color(&r->raster), bytes);
    for (int axis = 0; axis < 2; axis++) {
        render_view_t b = a;
        if (axis == 0) {
            b.small_fill.gate = fill.gate == 0 ? UINT8_MAX : 0;
        } else {
            b.small_fill.style = (fill.style + 1) % SMALL_FILL_STYLE_COUNT;
        }
        small_fill_reference(r, b.small_fill);
        raster_draw(&r->raster, &b);
        TEST_ASSERT_EQUAL_MEMORY(r->own, raster_color(&r->raster), bytes);
        raster_census(&r->raster, &a);
        raster_draw_culled(&r->raster, &a);
        TEST_ASSERT_EQUAL_MEMORY(expected, raster_color(&r->raster), bytes);
        raster_draw(&r->raster, &a);
        TEST_ASSERT_EQUAL_MEMORY(expected, raster_color(&r->raster), bytes);
    }
}

static void
test_view_small_fill_styles_and_frame_isolation(void) {
    raster_rig_t* r = small_fill_fixture(false);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.ceiling, r->view.small_fill.ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.gate, r->view.small_fill.gate);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_FLAT, r->view.small_fill.style);
    uint16_t* frames = (uint16_t*)r->own;
    const size_t bytes = W * H * sizeof(*frames);
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        r->view.small_fill = (small_fill_t){SMALL_FILL_CEILING_FAST, 0, style};
        raster_draw(&r->raster, &r->view);
        memcpy(frames + style * W * H, raster_color(&r->raster), bytes);
        for (int previous = SMALL_FILL_FLAT; previous < (int)style; previous++) {
            TEST_ASSERT_NOT_EQUAL(0, memcmp(frames + previous * W * H, frames + style * W * H, bytes));
        }
    }
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        r->view.small_fill = (small_fill_t){SMALL_FILL_CEILING_FAST, 0, style};
        raster_census(&r->raster, &r->view);
        raster_draw_culled(&r->raster, &r->view);
        TEST_ASSERT_EQUAL_MEMORY(frames + style * W * H, raster_color(&r->raster), bytes);
    }
}

static void
test_view_small_fill_frame_transition_limits(void) {
    raster_rig_t* r = small_fill_fixture(true);
    static const uint8_t ceilings[] = {0, R3D_SPAN_SMALL_MAX_SIDE, R3D_SPAN_SMALL_MAX_SIDE + 1, UINT8_MAX};
    static const uint8_t gates[] = {0, SMALL_FILL_GATE_FAST, UINT8_MAX};
    for (size_t ceiling = 0; ceiling < sizeof ceilings / sizeof ceilings[0]; ceiling++) {
        for (size_t gate = 0; gate < sizeof gates / sizeof gates[0]; gate++) {
            for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
                assert_small_fill_sequence(r, (small_fill_t){ceilings[ceiling], gates[gate], style});
            }
        }
    }
}

static void
test_split_state_forgets_every_share(void) {
    raster_split_t split;
    for (int i = 0; i < RASTER_SPLIT_INSTANCES; i++) {
        split.top_offset[i] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
        split.transform_offset[i] = JOB_SHARE_MAX - JOB_SHARE_EVEN;
    }
    split.resolve_offset = split.upscale_offset = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    raster_split_forget(&split);
    for (int i = 0; i < RASTER_SPLIT_INSTANCES; i++) {
        TEST_ASSERT_EQUAL_INT(0, split.top_offset[i]);
        TEST_ASSERT_EQUAL_INT(0, split.transform_offset[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, split.resolve_offset);
    TEST_ASSERT_EQUAL_INT(0, split.upscale_offset);
}

typedef struct {
    int top, even;
} split_rows_t;

static void
split_clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    memset(pixels, 0, count);
    const raster_buffers_t buffers = raster_carve(raster);
    if (pixels == gfx_target_row(&buffers.picture, GFX_ATTACHMENT_FURTHER, 0)) {
        split_rows_t* rows = self->state;
        rows->top = (int)count / raster->width;
        rows->even = r3d_pipeline_draw_split(raster->instances[0].mesh, buffers.culled + 1, buffers.rows,
                                             buffers.culled[0], raster->height, JOB_SHARE_EVEN);
    }
}

static void
test_split_feedback_preserves_pixels_and_null_uses_even_rows(void) {
    split_rows_t rows = {0};
    const uint32_t factor = 1;
    const raster_attachment_t tags = {
        .format = IMAGE_U16, .clear = id_clear, .writer = id_writer, .state = (void*)&factor};
    const raster_attachment_t attachment = {.format = IMAGE_U16, .clear = split_clear, .state = &rows};
    const raster_attachment_t* attached[] = {&attachment, &tags};
    raster_rig_t* r = rig_open(attached, 2);
    draw(r);
    TEST_ASSERT_EQUAL_INT(rows.even, rows.top);
    const raster_buffers_t picture_buffers = raster_carve(&r->raster);
    const gfx_target_t* picture = &picture_buffers.picture;
    const size_t bytes = gfx_attachment_bytes(IMAGE_U16, W, H);
    void* expected = malloc(2 * bytes);
    TEST_ASSERT_NOT_NULL(expected);
    memcpy(expected, picture->attachment[GFX_ATTACHMENT_COLOR].pixels, bytes);
    memcpy((uint8_t*)expected + bytes, raster_rig_attachment(r, 1), bytes);
    raster_split_t split = {0};
    r->raster.split = &split;
    draw(r);
    TEST_ASSERT_EQUAL_INT(rows.even, rows.top);
    for (int share = JOB_SHARE_MIN; share <= JOB_SHARE_MAX; share += (JOB_SHARE_MAX - JOB_SHARE_MIN) / 2) {
        for (int i = 0; i < r->raster.instance_count; i++) {
            split.top_offset[i] = (int16_t)(share - JOB_SHARE_EVEN);
        }
        draw(r);
        TEST_ASSERT_TRUE(share < JOB_SHARE_EVEN    ? rows.top < rows.even
                         : share == JOB_SHARE_EVEN ? rows.top == rows.even
                                                   : rows.top > rows.even);
        TEST_ASSERT_EQUAL_MEMORY(expected, picture->attachment[GFX_ATTACHMENT_COLOR].pixels, bytes);
        TEST_ASSERT_EQUAL_MEMORY((uint8_t*)expected + bytes, raster_rig_attachment(r, 1), bytes);
    }
    free(expected);
}

#ifndef DEVICE_BUILD
static int* observed_top;

static void
observe_split_span(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int first, int last, int32_t z,
                   int32_t dz) {
    if (rows->first_row == 0) {
        observed_top[writer->value] = gfx_target_end_row(rows);
    }
}

static bool
observe_split_writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    out->span = observe_split_span;
    out->value = (uint32_t)instance;
    return true;
}

static void
test_draw_retains_only_the_drawn_instance_and_bounds_state(void) {
    const raster_attachment_t observer = {.format = IMAGE_U16, .writer = observe_split_writer};
    const raster_attachment_t* attached[] = {&observer};
    raster_rig_t* r = rig_open(attached, 1);
    r3d_instance_t instances[RASTER_SPLIT_INSTANCES + 1];
    for (int i = 0; i <= RASTER_SPLIT_INSTANCES; i++) {
        instances[i] = r->instance[0];
    }

    struct {
        raster_split_t split;
        int16_t sentinel;
    } state = {.sentinel = JOB_SHARE_MIN};

    r->raster.instances = instances;
    r->raster.instance_count = RASTER_SPLIT_INSTANCES + 1;
    free(r->raster.scratch);
    r->raster.scratch = malloc(raster_scratch_bytes(&r->raster));
    TEST_ASSERT_NOT_NULL(r->raster.scratch);
    r->raster.split = &state.split;
    int top_rows[RASTER_SPLIT_INSTANCES + 1] = {0};
    observed_top = top_rows;
    for (int i = 0; i < RASTER_SPLIT_INSTANCES; i++) {
        state.split.top_offset[i] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    }
    draw(r);
    const raster_buffers_t buffers = raster_carve(&r->raster);
    const int even = r3d_pipeline_draw_split(instances[0].mesh, buffers.culled + 1, buffers.rows, buffers.culled[0], H,
                                             JOB_SHARE_EVEN);
    TEST_ASSERT_EQUAL_INT(even, observed_top[RASTER_SPLIT_INSTANCES]);
    const int retained = r3d_pipeline_draw_split(instances[0].mesh, buffers.culled + 1, buffers.rows, buffers.culled[0],
                                                 H, JOB_SHARE_MIN);
    TEST_ASSERT_EQUAL_INT(retained, observed_top[RASTER_SPLIT_INSTANCES - 1]);
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MIN, state.sentinel);

    r->raster.instance_count = 2;
    const raster_split_t before = state.split;
    memset(top_rows, 0, sizeof top_rows);
    draw(r);
    for (int i = 0; i < r->raster.instance_count; i++) {
        TEST_ASSERT_GREATER_THAN_INT(0, observed_top[i]);
        TEST_ASSERT_INT_WITHIN((JOB_SHARE_MAX - JOB_SHARE_MIN) / 2, 0, state.split.top_offset[i]);
    }
    for (int i = 2; i < RASTER_SPLIT_INSTANCES; i++) {
        TEST_ASSERT_EQUAL_INT(0, observed_top[i]);
        TEST_ASSERT_EQUAL_INT(before.top_offset[i], state.split.top_offset[i]);
    }
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MIN, state.sentinel);
}
#endif

static void
test_context_owns_and_forgets_split(void) {
    raster_rig_t* r = rig_open(NULL, 0);
    render_context_t context = {.scale_percent = 100};
    const render_view_t view = render_view_fixture(&raster_rig_camera, &r->raster, 0);
    TEST_ASSERT_TRUE(render_context_draw(&context, r->instance, 2, &view, 0));
    TEST_ASSERT_EQUAL_PTR(&context.split, context.raster.split);
    context.split.top_offset[0] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    render_context_forget(&context);
    TEST_ASSERT_EQUAL_INT(0, context.split.top_offset[0]);
    context.split.top_offset[RASTER_SPLIT_INSTANCES - 1] = JOB_SHARE_MAX - JOB_SHARE_EVEN;
    render_context_release(&context);
    for (int i = 0; i < RASTER_SPLIT_INSTANCES; i++) {
        TEST_ASSERT_EQUAL_INT(0, context.split.top_offset[i]);
    }
}

void
run_raster_attachment_suite(void) {
    RUN_TEST(test_context_owns_and_forgets_split);
#ifndef DEVICE_BUILD
    RUN_TEST(test_draw_retains_only_the_drawn_instance_and_bounds_state);
#endif
    RUN_TEST(test_split_state_forgets_every_share);
    RUN_TEST(test_split_feedback_preserves_pixels_and_null_uses_even_rows);
    RUN_TEST(test_view_small_fill_styles_and_frame_isolation);
    RUN_TEST(test_view_small_fill_frame_transition_limits);
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
