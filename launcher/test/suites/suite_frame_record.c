/* frame_record against a render context drawing the raster rig: the stream
 * opens with its header, keyframes carry every mesh and the frames between
 * only what moved, a frame the sink refuses is dropped whole and counted
 * with the next one a keyframe, a changed tunable rides in the next frame,
 * a keyframe's composed picture is hashed into a CHECK that waits for a
 * frame the sink takes, a generated frame is never a keyframe, and a mesh
 * the app made carries what it was made from (a pose's palette, a build's
 * source) in every instance drawing it, its note giving way when full or
 * when its block is made over. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "core/tune.h"
#include "hash/crc32.h"
#include "math/linear/mat4f.h"
#include "render/context/render_context.h"
#include "render/record/frame_record.h"
#include "render/record/frame_record_format.h"

#define W              64
#define H              48
/* Short, so a few frames hold two keyframe intervals. */
#define KEYFRAME_EVERY 4
/* Every test's whole stream: a rig frame is a few hundred bytes. */
#define SINK_BYTES     (32 * 1024)
#define RECORDS_MAX    256
#define DT_MS          16

typedef struct {
    uint8_t* bytes;
    size_t size;
    int writes;
    int refused; /* this write (0 is the header) is refused, or -1 */
} sink_t;

typedef struct {
    uint16_t type;
    const uint8_t* body;
    uint32_t length;
} found_t;

static sink_t* sink;
static render_context_t* context;
static raster_rig_t* rig;
static found_t found[RECORDS_MAX];
static int found_count;

static bool
take(void* state, const uint8_t* bytes, size_t count) {
    sink_t* s = state;
    const int n = s->writes++;
    if (n == s->refused) {
        return false;
    }
    TEST_ASSERT_TRUE(s->size + count <= SINK_BYTES);
    memcpy(s->bytes + s->size, bytes, count);
    s->size += count;
    return true;
}

static void
release(void) {
    frame_record_stop();
    if (sink != NULL) {
        free(sink->bytes);
    }
    free(sink);
    sink = NULL;
    if (context != NULL) {
        render_context_release(context);
    }
    free(context);
    context = NULL;
    raster_rig_release();
}

/* The rig's two quads, both placed, recording from here with `refused` the
 * write the sink turns down. */
static void
open_recording(int refused) {
    rig = raster_rig_open(raster_rig_wall_and_card, RASTER_RIG_QUADS, 3U, W, H, 0);
    suite_set_test_cleanup(release);
    for (int i = 0; i < RASTER_RIG_QUADS; i++) {
        rig->model[i] = mat4f_identity();
    }
    context = calloc(1, sizeof(*context)); /* too big for the device stack */
    sink = calloc(1, sizeof(*sink));
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_NOT_NULL(sink);
    sink->bytes = malloc(SINK_BYTES);
    TEST_ASSERT_NOT_NULL(sink->bytes);
    sink->refused = refused;
    render_context_set_scale(context, 100);
    const frame_record_out_t out = {take, sink};
    TEST_ASSERT_TRUE(frame_record_start(&out, KEYFRAME_EVERY));
}

static void
draw_frame(void) {
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(context, rig->instance, RASTER_RIG_QUADS, &view, 0));
    frame_record_frame(context, &context->raster, DT_MS);
}

/* Every record in what the sink took, in order. */
static void
find_records(void) {
    found_count = 0;
    for (size_t at = 0; at < sink->size;) {
        frame_record_head_t head;
        TEST_ASSERT_TRUE(at + sizeof head <= sink->size);
        memcpy(&head, sink->bytes + at, sizeof head);
        at += sizeof head;
        TEST_ASSERT_TRUE(at + head.length <= sink->size);
        TEST_ASSERT_TRUE(found_count < RECORDS_MAX);
        found[found_count++] = (found_t){head.type, sink->bytes + at, head.length};
        at += head.length;
    }
}

/* The k-th FRAME record's position in `found`, or -1. */
static int
frame_at(int k) {
    for (int i = 0; i < found_count; i++) {
        if (found[i].type == FRAME_RECORD_FRAME && k-- == 0) {
            return i;
        }
    }
    return -1;
}

static frame_record_frame_t
frame_of(int k) {
    const int at = frame_at(k);
    TEST_ASSERT_TRUE(at >= 0);
    frame_record_frame_t frame;
    memcpy(&frame, found[at].body, sizeof frame);
    return frame;
}

/* How many records of `type` the k-th frame holds. */
static int
count_in_frame(int k, uint16_t type) {
    const int at = frame_at(k);
    TEST_ASSERT_TRUE(at >= 0);
    int count = 0;
    for (int i = at + 1; i < found_count && found[i].type != FRAME_RECORD_FRAME; i++) {
        count += found[i].type == type;
    }
    return count;
}

/* The first record of `type` in the k-th frame. */
static const found_t*
first_in_frame(int k, uint16_t type) {
    for (int i = frame_at(k) + 1; i < found_count && found[i].type != FRAME_RECORD_FRAME; i++) {
        if (found[i].type == type) {
            return &found[i];
        }
    }
    TEST_FAIL_MESSAGE("no such record in the frame");
    return NULL;
}

static void
test_a_stream_opens_with_its_header_then_each_frame_with_frame_view_and_choice(void) {
    open_recording(-1);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT16(FRAME_RECORD_HEADER, found[0].type);
    frame_record_header_t header;
    TEST_ASSERT_EQUAL_UINT32(sizeof header, found[0].length);
    memcpy(&header, found[0].body, sizeof header);
    TEST_ASSERT_EQUAL_UINT16(FRAME_RECORD_VERSION, header.version);
    TEST_ASSERT_EQUAL_UINT16(KEYFRAME_EVERY, header.keyframe_every);
    TEST_ASSERT_EQUAL_UINT16(FRAME_RECORD_FRAME, found[1].type);
    TEST_ASSERT_EQUAL_UINT16(FRAME_RECORD_VIEW, found[2].type);
    TEST_ASSERT_EQUAL_UINT32(sizeof(render_view_t), found[2].length);
    TEST_ASSERT_EQUAL_UINT16(FRAME_RECORD_CHOICE, found[3].type);
    const frame_record_frame_t frame = frame_of(0);
    TEST_ASSERT_EQUAL_UINT32(0, frame.index);
    TEST_ASSERT_EQUAL_UINT32(DT_MS, frame.dt_ms);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME, frame.flags);
    TEST_ASSERT_EQUAL_UINT16(RASTER_RIG_QUADS, frame.instances);
}

static void
test_keyframes_carry_every_mesh_and_the_frames_between_only_one_that_moved(void) {
    open_recording(-1);
    for (int i = 0; i < KEYFRAME_EVERY + 1; i++) {
        if (i == 2) {
            rig->model[1].m[0][0] = 2.0F; /* any change of its place */
        }
        draw_frame();
    }
    find_records();
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(0, FRAME_RECORD_MESH));
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(1, FRAME_RECORD_MESH));
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(2, FRAME_RECORD_MESH));
    frame_record_mesh_t moved;
    memcpy(&moved, first_in_frame(2, FRAME_RECORD_MESH)->body, sizeof moved);
    TEST_ASSERT_EQUAL_UINT16(1, moved.index);
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(3, FRAME_RECORD_MESH));
    TEST_ASSERT_EQUAL_UINT8(0, frame_of(3).flags);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME, frame_of(KEYFRAME_EVERY).flags);
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(KEYFRAME_EVERY, FRAME_RECORD_MESH));
}

static void
test_a_refused_frame_is_dropped_whole_and_counted_and_the_next_is_a_keyframe(void) {
    open_recording(2); /* the header, frame 0, then frame 1 */
    for (int i = 0; i < 3; i++) {
        draw_frame();
    }
    const frame_record_counts_t counts = frame_record_counts();
    TEST_ASSERT_EQUAL_UINT32(2, counts.frames);
    TEST_ASSERT_EQUAL_UINT32(1, counts.dropped);
    find_records();
    TEST_ASSERT_EQUAL_UINT32(0, frame_of(0).index);
    const frame_record_frame_t after = frame_of(1);
    TEST_ASSERT_EQUAL_UINT32(2, after.index);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME | FRAME_RECORD_AFTER_DROP, after.flags);
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(1, FRAME_RECORD_MESH));
    TEST_ASSERT_EQUAL_INT(-1, frame_at(2));
}

static const tune_entry_t* knob;

static void
reset_knob(void) {
    (void)tune_reset(tune_shared(), knob->name);
    release();
}

static void
test_a_changed_tunable_rides_in_the_next_frame_and_every_keyframe_while_off_its_initial(void) {
    knob = tune_find(tune_shared(), TUNE_NAME(render, cull));
    TEST_ASSERT_NOT_NULL(knob);
    open_recording(-1);
    suite_set_test_cleanup(reset_knob);
    const int32_t value = knob->initial != knob->low ? knob->low : knob->high;
    for (int i = 0; i < KEYFRAME_EVERY + 1; i++) {
        if (i == 1) {
            TEST_ASSERT_TRUE(tune_set(tune_shared(), knob->name, value));
        }
        draw_frame();
    }
    find_records();
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(0, FRAME_RECORD_TUNE));
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(1, FRAME_RECORD_TUNE));
    frame_record_tune_t tune;
    memcpy(&tune, first_in_frame(1, FRAME_RECORD_TUNE)->body, sizeof tune);
    TEST_ASSERT_EQUAL_STRING_LEN(knob->name, tune.name, strlen(knob->name));
    TEST_ASSERT_EQUAL_INT32(value, tune.value);
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(2, FRAME_RECORD_TUNE));
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(KEYFRAME_EVERY, FRAME_RECORD_TUNE));
}

static void
test_a_keyframes_picture_is_hashed_into_the_next_frame_and_the_last_one_at_stop(void) {
    open_recording(-1);
    uint16_t* picture = calloc((size_t)W * H, sizeof *picture);
    TEST_ASSERT_NOT_NULL(picture);
    picture[7] = 0xBEEF;
    const uint32_t crc = crc32_update(0, picture, (size_t)W * H * sizeof *picture);
    for (int i = 0; i < KEYFRAME_EVERY + 1; i++) {
        draw_frame();
        frame_record_note_shown(picture, W, H);
    }
    free(picture);
    frame_record_stop();
    find_records();
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(0, FRAME_RECORD_CHECK));
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(1, FRAME_RECORD_CHECK));
    frame_record_check_t check;
    memcpy(&check, first_in_frame(1, FRAME_RECORD_CHECK)->body, sizeof check);
    TEST_ASSERT_EQUAL_UINT32(0, check.index);
    TEST_ASSERT_EQUAL_UINT16(W, check.width);
    TEST_ASSERT_EQUAL_UINT16(H, check.height);
    TEST_ASSERT_EQUAL_HEX32(crc, check.crc32);
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(2, FRAME_RECORD_CHECK));
    /* The last keyframe's hash, written at stop after the last frame. */
    TEST_ASSERT_EQUAL_UINT16(FRAME_RECORD_CHECK, found[found_count - 1].type);
    memcpy(&check, found[found_count - 1].body, sizeof check);
    TEST_ASSERT_EQUAL_UINT32(KEYFRAME_EVERY, check.index);
}

static void
test_a_mesh_in_no_pack_is_named_by_no_pack_and_no_pack_is_named(void) {
    open_recording(-1);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(0, FRAME_RECORD_PACK));
    frame_record_mesh_t mesh;
    memcpy(&mesh, first_in_frame(0, FRAME_RECORD_MESH)->body, sizeof mesh);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_STATIC, mesh.kind);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_NO_PACK, mesh.positions.pack);
    TEST_ASSERT_EQUAL_HEX32(FRAME_RECORD_NO_ENTRY, mesh.positions.entry);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_NO_PACK, mesh.batch.pack);
}

static void
forget_notes(void) {
    frame_record_forget_notes();
    release();
}

/* The k-th frame's record of `type` for mesh `index`, the index its first field. */
static const found_t*
mesh_record(int k, uint16_t type, uint16_t index) {
    for (int i = frame_at(k) + 1; i < found_count && found[i].type != FRAME_RECORD_FRAME; i++) {
        uint16_t at;
        memcpy(&at, found[i].body, sizeof at);
        if (found[i].type == type && at == index) {
            return &found[i];
        }
    }
    return NULL;
}

static void
test_a_posed_mesh_names_its_bind_and_its_palette_rides_each_keyframe_and_each_new_pose(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    mat4f_t palette[2] = {mat4f_identity(), mat4f_identity()};
    palette[1].m[3][0] = 0.25F;
    const r3d_lit_mesh_t* posed = &rig->quad[0].mesh;
    const r3d_lit_mesh_t* bind = &rig->quad[1].mesh; /* the same vertex count, elsewhere */
    frame_record_note_skin(posed, bind, palette, 2);
    draw_frame();
    draw_frame();
    frame_record_note_skin(posed, bind, palette, 2);
    draw_frame();
    find_records();
    frame_record_mesh_t mesh;
    memcpy(&mesh, mesh_record(0, FRAME_RECORD_MESH, 0)->body, sizeof mesh);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_SKINNED, mesh.kind);
    const found_t* pose = mesh_record(0, FRAME_RECORD_POSE, 0);
    TEST_ASSERT_NOT_NULL(pose);
    frame_record_pose_t head;
    memcpy(&head, pose->body, sizeof head);
    TEST_ASSERT_EQUAL_UINT16(2, head.joints);
    TEST_ASSERT_EQUAL_UINT32(sizeof head + sizeof palette, pose->length);
    TEST_ASSERT_EQUAL_MEMORY(palette, pose->body + sizeof head, sizeof palette);
    TEST_ASSERT_NULL(mesh_record(1, FRAME_RECORD_POSE, 0));
    TEST_ASSERT_NOT_NULL(mesh_record(2, FRAME_RECORD_POSE, 0));
    TEST_ASSERT_NULL(mesh_record(0, FRAME_RECORD_POSE, 1));
}

/* One built mesh's source: a triangle. */
static const vec3f_t triangle_positions[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
static const uint8_t triangle_colours[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
static const uint16_t triangle_indices[1][3] = {{0, 1, 2}};
static const r3d_dynamic_source_t triangle = {triangle_positions, triangle_colours, 3, triangle_indices, 1, true};

static void
test_a_built_mesh_carries_its_source_whole_each_keyframe_and_each_build(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    frame_record_note_built(&rig->quad[1].mesh, &triangle, true);
    draw_frame();
    draw_frame();
    find_records();
    frame_record_mesh_t mesh;
    memcpy(&mesh, mesh_record(0, FRAME_RECORD_MESH, 1)->body, sizeof mesh);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_BUILT, mesh.kind);
    const found_t* dyn = mesh_record(0, FRAME_RECORD_DYNMESH, 1);
    TEST_ASSERT_NOT_NULL(dyn);
    frame_record_dynmesh_t head;
    memcpy(&head, dyn->body, sizeof head);
    TEST_ASSERT_EQUAL_UINT32(3, head.vertices);
    TEST_ASSERT_EQUAL_UINT32(1, head.triangles);
    TEST_ASSERT_EQUAL_UINT8(1, head.fitted);
    TEST_ASSERT_EQUAL_UINT8(1, head.double_sided);
    TEST_ASSERT_EQUAL_UINT32(
        sizeof head + sizeof triangle_positions + sizeof triangle_colours + sizeof triangle_indices, dyn->length);
    const uint8_t* at = dyn->body + sizeof head;
    TEST_ASSERT_EQUAL_MEMORY(triangle_positions, at, sizeof triangle_positions);
    TEST_ASSERT_EQUAL_MEMORY(triangle_colours, at + sizeof triangle_positions, sizeof triangle_colours);
    TEST_ASSERT_EQUAL_MEMORY(triangle_indices, at + sizeof triangle_positions + sizeof triangle_colours,
                             sizeof triangle_indices);
    TEST_ASSERT_NULL(mesh_record(1, FRAME_RECORD_DYNMESH, 1));
}

/* The frame the context just drew, recorded as a generated one: it reuses the
 * last drawn frame's picture and draws no meshes. */
static void
generate_frame(void) {
    context->frame.generated = true;
    frame_record_frame(context, &context->raster, DT_MS);
    context->frame.generated = false;
}

static void
test_a_generated_frame_on_a_keyframes_turn_hands_the_keyframe_to_the_next_drawn_frame(void) {
    open_recording(-1);
    for (int i = 0; i < KEYFRAME_EVERY; i++) {
        draw_frame();
    }
    generate_frame();
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_GENERATED, frame_of(KEYFRAME_EVERY).flags);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME, frame_of(KEYFRAME_EVERY + 1).flags);
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(KEYFRAME_EVERY + 1, FRAME_RECORD_MESH));
}

static void
test_a_generated_frame_after_a_drop_is_no_keyframe_and_the_next_drawn_frame_is(void) {
    open_recording(2); /* the header, frame 0, then frame 1 */
    draw_frame();
    draw_frame();
    generate_frame();
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_AFTER_DROP | FRAME_RECORD_GENERATED, frame_of(1).flags);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME, frame_of(2).flags);
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(2, FRAME_RECORD_MESH));
}

static void
test_a_hash_whose_frame_is_dropped_rides_the_next_frame_taken(void) {
    open_recording(2); /* the header, frame 0, then frame 1, which carries frame 0's hash */
    static const uint16_t picture[W * H];
    draw_frame();
    frame_record_note_shown(picture, W, H);
    draw_frame();
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT32(2, frame_of(1).index);
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(1, FRAME_RECORD_CHECK));
    frame_record_check_t check;
    memcpy(&check, first_in_frame(1, FRAME_RECORD_CHECK)->body, sizeof check);
    TEST_ASSERT_EQUAL_UINT32(0, check.index);
}

static void
test_a_mesh_struct_rewritten_in_place_with_new_arrays_is_recorded_again(void) {
    open_recording(-1);
    draw_frame();
    rig->quad[1].mesh.positions = rig->quad[0].mesh.positions; /* the same struct, another array */
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(1, FRAME_RECORD_MESH));
    TEST_ASSERT_NOT_NULL(mesh_record(1, FRAME_RECORD_MESH, 1));
}

static void
test_each_instance_drawing_a_rebuilt_mesh_carries_the_build(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    rig->instance[1].mesh = rig->instance[0].mesh;
    frame_record_note_built(rig->instance[0].mesh, &triangle, true);
    draw_frame();
    draw_frame();
    frame_record_note_built(rig->instance[0].mesh, &triangle, true);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(1, FRAME_RECORD_DYNMESH));
    TEST_ASSERT_NOT_NULL(mesh_record(2, FRAME_RECORD_DYNMESH, 0));
    TEST_ASSERT_NOT_NULL(mesh_record(2, FRAME_RECORD_DYNMESH, 1));
}

static void
test_a_build_before_a_generated_frame_rides_the_next_drawn_frame(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    frame_record_note_built(&rig->quad[1].mesh, &triangle, true);
    draw_frame();
    draw_frame();
    frame_record_note_built(&rig->quad[1].mesh, &triangle, true);
    generate_frame();
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(2, FRAME_RECORD_DYNMESH));
    TEST_ASSERT_NOT_NULL(mesh_record(3, FRAME_RECORD_DYNMESH, 1));
}

static uint8_t
kind_of(int k, uint16_t index) {
    frame_record_mesh_t mesh;
    memcpy(&mesh, mesh_record(k, FRAME_RECORD_MESH, index)->body, sizeof mesh);
    return mesh.kind;
}

static void
test_a_full_note_table_gives_its_oldest_note_to_a_new_build(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    static int16_t elsewhere[FRAME_RECORD_NOTED_BUILT_MAX][3];
    for (int i = 0; i < FRAME_RECORD_NOTED_BUILT_MAX; i++) {
        const r3d_lit_mesh_t gone = {.positions = &elsewhere[i], .vertex_count = 3};
        frame_record_note_built(&gone, &triangle, true);
    }
    frame_record_note_built(&rig->quad[1].mesh, &triangle, true);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_BUILT, kind_of(0, 1));
}

static void
test_a_mesh_made_in_a_block_forgets_the_notes_of_meshes_that_lived_there(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    const r3d_lit_mesh_t* mesh = &rig->quad[1].mesh;
    frame_record_note_built(mesh, &triangle, true);
    frame_record_note_skin(&rig->quad[0].mesh, mesh, NULL, 0);
    frame_record_forget(mesh->positions, sizeof mesh->positions[0]);
    frame_record_forget(rig->quad[0].mesh.positions, sizeof mesh->positions[0]);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_STATIC, kind_of(0, 0));
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_STATIC, kind_of(0, 1));
}

static void
test_a_second_start_is_refused_while_recording(void) {
    open_recording(-1);
    const frame_record_out_t out = {take, sink};
    TEST_ASSERT_FALSE(frame_record_start(&out, KEYFRAME_EVERY));
    TEST_ASSERT_TRUE(frame_record_active());
    frame_record_stop();
    TEST_ASSERT_FALSE(frame_record_active());
}

void
run_frame_record_suite(void) {
    RUN_TEST(test_a_stream_opens_with_its_header_then_each_frame_with_frame_view_and_choice);
    RUN_TEST(test_keyframes_carry_every_mesh_and_the_frames_between_only_one_that_moved);
    RUN_TEST(test_a_refused_frame_is_dropped_whole_and_counted_and_the_next_is_a_keyframe);
    RUN_TEST(test_a_changed_tunable_rides_in_the_next_frame_and_every_keyframe_while_off_its_initial);
    RUN_TEST(test_a_keyframes_picture_is_hashed_into_the_next_frame_and_the_last_one_at_stop);
    RUN_TEST(test_a_second_start_is_refused_while_recording);
    RUN_TEST(test_a_mesh_in_no_pack_is_named_by_no_pack_and_no_pack_is_named);
    RUN_TEST(test_a_posed_mesh_names_its_bind_and_its_palette_rides_each_keyframe_and_each_new_pose);
    RUN_TEST(test_a_built_mesh_carries_its_source_whole_each_keyframe_and_each_build);
    RUN_TEST(test_a_generated_frame_on_a_keyframes_turn_hands_the_keyframe_to_the_next_drawn_frame);
    RUN_TEST(test_a_generated_frame_after_a_drop_is_no_keyframe_and_the_next_drawn_frame_is);
    RUN_TEST(test_a_hash_whose_frame_is_dropped_rides_the_next_frame_taken);
    RUN_TEST(test_a_mesh_struct_rewritten_in_place_with_new_arrays_is_recorded_again);
    RUN_TEST(test_each_instance_drawing_a_rebuilt_mesh_carries_the_build);
    RUN_TEST(test_a_build_before_a_generated_frame_rides_the_next_drawn_frame);
    RUN_TEST(test_a_full_note_table_gives_its_oldest_note_to_a_new_build);
    RUN_TEST(test_a_mesh_made_in_a_block_forgets_the_notes_of_meshes_that_lived_there);
}

SUITE_REGISTER(run_frame_record_suite);
