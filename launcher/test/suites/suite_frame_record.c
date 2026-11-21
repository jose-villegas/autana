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

#ifndef DEVICE_BUILD
#include <stdio.h>

#include "asset/asset_store.h"
#include "test_asset_dir.h"
#include "test_pack.h"
#endif

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
static found_t* found; /* RECORDS_MAX, taken per test: on-chip RAM is scarce */
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
    free(found);
    found = NULL;
    found_count = 0;
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
    found = calloc(RECORDS_MAX, sizeof(*found));
    TEST_ASSERT_NOT_NULL(found);
    sink->refused = refused;
    render_context_set_scale(context, 100);
    const frame_record_out_t out = {take, sink};
    TEST_ASSERT_TRUE(frame_record_start(&out, KEYFRAME_EVERY));
}

static void
draw_frame(void) {
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(context, rig->instance, RASTER_RIG_QUADS, &view, 0));
    render_context_record_frame(context, DT_MS);
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
    frame_record_choice_t choice;
    TEST_ASSERT_EQUAL_UINT32(sizeof choice, found[3].length);
    memcpy(&choice, found[3].body, sizeof choice);
    /* The size it drew at: scale 100 draws the whole viewport. */
    TEST_ASSERT_EQUAL_UINT16(W, choice.width);
    TEST_ASSERT_EQUAL_UINT16(H, choice.height);
    const frame_record_frame_t frame = frame_of(0);
    TEST_ASSERT_EQUAL_UINT32(0, frame.index);
    TEST_ASSERT_EQUAL_UINT32(DT_MS, frame.dt_ms);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME, frame.flags);
    TEST_ASSERT_EQUAL_UINT16(RASTER_RIG_QUADS, frame.instances);
    /* The records after its FRAME, to the stream's end: it is the only frame. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(sink->bytes + sink->size - (found[1].body + found[1].length)), frame.bytes);
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

/* How many TUNE records in the k-th frame name `name`, the last one's value in `value`. Other tunables may be
 * off their initial values already, as a booted board's settings leave them. */
static int
tunes_named(int k, const char* name, int32_t* value) {
    const int at = frame_at(k);
    TEST_ASSERT_TRUE(at >= 0);
    int count = 0;
    for (int i = at + 1; i < found_count && found[i].type != FRAME_RECORD_FRAME; i++) {
        frame_record_tune_t tune;
        if (found[i].type != FRAME_RECORD_TUNE) {
            continue;
        }
        memcpy(&tune, found[i].body, sizeof tune);
        if (strncmp(tune.name, name, sizeof tune.name) == 0) {
            *value = tune.value;
            count++;
        }
    }
    return count;
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
    int32_t recorded = 0;
    TEST_ASSERT_EQUAL_INT(0, tunes_named(0, knob->name, &recorded));
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(1, FRAME_RECORD_TUNE));
    TEST_ASSERT_EQUAL_INT(1, tunes_named(1, knob->name, &recorded));
    TEST_ASSERT_EQUAL_INT32(value, recorded);
    TEST_ASSERT_EQUAL_INT(0, count_in_frame(2, FRAME_RECORD_TUNE));
    recorded = 0;
    TEST_ASSERT_EQUAL_INT(1, tunes_named(KEYFRAME_EVERY, knob->name, &recorded));
    TEST_ASSERT_EQUAL_INT32(value, recorded);
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

/* Frame 0's record of `type` for mesh `index`, whose MESH record names it `kind`. */
static const found_t*
carried(uint16_t index, uint8_t kind, uint16_t type) {
    frame_record_mesh_t mesh;
    memcpy(&mesh, mesh_record(0, FRAME_RECORD_MESH, index)->body, sizeof mesh);
    TEST_ASSERT_EQUAL_UINT8(kind, mesh.kind);
    const found_t* found_record = mesh_record(0, type, index);
    TEST_ASSERT_NOT_NULL(found_record);
    return found_record;
}

static void
test_a_posed_mesh_names_its_bind_and_its_palette_rides_each_keyframe_and_each_new_pose(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    mat4f_t palette[2] = {mat4f_identity(), mat4f_identity()};
    palette[1].m[3][0] = 0.25F;
    const r3d_lit_mesh_t* posed = &rig->quad[0].mesh;
    const r3d_lit_mesh_t* bind = &rig->quad[1].mesh; /* the same vertex count, elsewhere */
    /* No pack holds the rig's skin: the POSE names none. */
    const r3d_skin_t skin = {.joint_count = 2, .vertices = (const uint8_t*)palette};
    frame_record_note_skin(posed, bind, &skin, palette, 2);
    draw_frame();
    draw_frame();
    frame_record_note_skin(posed, bind, &skin, palette, 2);
    draw_frame();
    find_records();
    const found_t* pose = carried(0, FRAME_RECORD_SKINNED, FRAME_RECORD_POSE);
    frame_record_pose_t head;
    memcpy(&head, pose->body, sizeof head);
    TEST_ASSERT_EQUAL_UINT16(2, head.joints);
    /* The rig's colours are its own, no pack's: they ride as the pose lit them. */
    TEST_ASSERT_NOT_NULL(posed->colors);
    TEST_ASSERT_EQUAL_UINT16(posed->vertex_count, head.lit);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_NO_PACK, head.skin.pack);
    const size_t colours = (size_t)head.lit * sizeof posed->colors[0];
    TEST_ASSERT_EQUAL_UINT32(sizeof head + sizeof palette + colours, pose->length);
    TEST_ASSERT_EQUAL_MEMORY(palette, pose->body + sizeof head, sizeof palette);
    TEST_ASSERT_EQUAL_MEMORY(posed->colors, pose->body + sizeof head + sizeof palette, colours);
    TEST_ASSERT_NULL(mesh_record(1, FRAME_RECORD_POSE, 0));
    TEST_ASSERT_NOT_NULL(mesh_record(2, FRAME_RECORD_POSE, 0));
    TEST_ASSERT_NULL(mesh_record(0, FRAME_RECORD_POSE, 1));
}

/* One built mesh's source: a triangle. */
static const vec3f_t triangle_positions[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
static const uint8_t triangle_colours[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
static const uint16_t triangle_indices[1][3] = {{0, 1, 2}};
static const r3d_mesh_source_t triangle = {triangle_positions, triangle_colours, 3, triangle_indices, 1, true, NULL};
/* The same triangle textured: its UVs in LUVS's form. */
static const int16_t triangle_uvs[3][2] = {{0, 0}, {256, 0}, {0, 256}};
static const r3d_mesh_source_t textured_triangle = {triangle_positions, triangle_colours, 3, triangle_indices, 1, false,
                                                    triangle_uvs};

static void
test_a_built_mesh_carries_its_source_whole_each_keyframe_and_each_build(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    frame_record_note_built(&rig->quad[1].mesh, &triangle, true);
    draw_frame();
    draw_frame();
    find_records();
    const found_t* dyn = carried(1, FRAME_RECORD_BUILT, FRAME_RECORD_DYNMESH);
    frame_record_dynmesh_t head;
    memcpy(&head, dyn->body, sizeof head);
    TEST_ASSERT_EQUAL_UINT32(3, head.vertices);
    TEST_ASSERT_EQUAL_UINT32(1, head.triangles);
    TEST_ASSERT_EQUAL_UINT8(1, head.fitted);
    TEST_ASSERT_EQUAL_UINT8(1, head.double_sided);
    TEST_ASSERT_EQUAL_UINT8(0, head.textured);
    TEST_ASSERT_EQUAL_UINT32(
        sizeof head + sizeof triangle_positions + sizeof triangle_colours + sizeof triangle_indices, dyn->length);
    const uint8_t* at = dyn->body + sizeof head;
    TEST_ASSERT_EQUAL_MEMORY(triangle_positions, at, sizeof triangle_positions);
    TEST_ASSERT_EQUAL_MEMORY(triangle_colours, at + sizeof triangle_positions, sizeof triangle_colours);
    TEST_ASSERT_EQUAL_MEMORY(triangle_indices, at + sizeof triangle_positions + sizeof triangle_colours,
                             sizeof triangle_indices);
    TEST_ASSERT_NULL(mesh_record(1, FRAME_RECORD_DYNMESH, 1));
}

static void
test_a_textured_built_mesh_carries_its_uvs_after_its_triangles(void) {
    open_recording(-1);
    suite_set_test_cleanup(forget_notes);
    frame_record_note_built(&rig->quad[1].mesh, &textured_triangle, false);
    draw_frame();
    find_records();
    const found_t* dyn = carried(1, FRAME_RECORD_BUILT, FRAME_RECORD_DYNMESH);
    frame_record_dynmesh_t head;
    memcpy(&head, dyn->body, sizeof head);
    TEST_ASSERT_EQUAL_UINT8(1, head.textured);
    TEST_ASSERT_EQUAL_UINT8(0, head.double_sided);
    const size_t before_uvs =
        sizeof head + sizeof triangle_positions + sizeof triangle_colours + sizeof triangle_indices;
    TEST_ASSERT_EQUAL_UINT32(before_uvs + sizeof triangle_uvs, dyn->length);
    TEST_ASSERT_EQUAL_MEMORY(triangle_uvs, dyn->body + before_uvs, sizeof triangle_uvs);
}

/* The frame the context just drew, recorded as a generated one: it reuses the
 * last drawn frame's picture and draws no meshes. */
static void
generate_frame(void) {
    context->frame.generated = true;
    render_context_record_frame(context, DT_MS);
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
test_generated_frames_after_a_drop_wait_for_a_drawn_keyframe(void) {
    open_recording(2); /* the header, frame 0, then frame 1 */
    draw_frame();
    draw_frame();
    generate_frame();
    generate_frame();
    draw_frame();
    generate_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT32(3, frame_record_counts().dropped);
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_AFTER_DROP | FRAME_RECORD_KEYFRAME, frame_of(1).flags);
    TEST_ASSERT_EQUAL_UINT32(4, frame_of(1).index);
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(1, FRAME_RECORD_MESH));
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_GENERATED, frame_of(2).flags);
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
    const r3d_skin_t skin = {0};
    frame_record_note_skin(&rig->quad[0].mesh, mesh, &skin, NULL, 0);
    frame_record_forget(mesh->positions, sizeof mesh->positions[0]);
    frame_record_forget(rig->quad[0].mesh.positions, sizeof mesh->positions[0]);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_STATIC, kind_of(0, 0));
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_STATIC, kind_of(0, 1));
}

static void
test_a_keyframe_without_a_snapshot_drops_instead_of_reusing_lod_state(void) {
    open_recording(-1);
    for (int i = 0; i < KEYFRAME_EVERY; i++) {
        draw_frame();
    }
    const size_t before = sink->size;
    render_context_record_frame(context, DT_MS);
    TEST_ASSERT_EQUAL_UINT32(before, sink->size);
    TEST_ASSERT_EQUAL_UINT32(1, frame_record_counts().dropped);
    draw_frame();
    find_records();
    TEST_ASSERT_EQUAL_UINT8(FRAME_RECORD_KEYFRAME | FRAME_RECORD_AFTER_DROP, frame_of(KEYFRAME_EVERY).flags);
    TEST_ASSERT_EQUAL_INT(1, count_in_frame(KEYFRAME_EVERY, FRAME_RECORD_LODSTATE));
}

static frame_record_choice_t
choice_of(int k) {
    frame_record_choice_t choice;
    const found_t* record = first_in_frame(k, FRAME_RECORD_CHOICE);
    TEST_ASSERT_EQUAL_UINT32(sizeof choice, record->length);
    memcpy(&choice, record->body, sizeof choice);
    return choice;
}

/* A drawn frame at scale 50, its step set off the fixed scale's -1 by hand, then the generated frame after it. */
static void
test_a_choice_carries_the_contexts_own_step_scale_size_and_generated_flag(void) {
    open_recording(-1);
    render_context_set_scale(context, 50);
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(context, rig->instance, RASTER_RIG_QUADS, &view, 0));
    context->frame.step = 2;
    const int drawn_width = context->frame.width;
    const int drawn_height = context->frame.height;
    TEST_ASSERT_EQUAL_INT(W / 2, drawn_width);
    TEST_ASSERT_EQUAL_INT(H / 2, drawn_height);
    render_context_record_frame(context, DT_MS);
    generate_frame();
    find_records();
    const frame_record_choice_t drawn = choice_of(0);
    TEST_ASSERT_EQUAL_INT16(2, drawn.step);
    TEST_ASSERT_EQUAL_UINT8(50, drawn.scale_percent);
    TEST_ASSERT_EQUAL_UINT16(drawn_width, drawn.width);
    TEST_ASSERT_EQUAL_UINT16(drawn_height, drawn.height);
    TEST_ASSERT_EQUAL_UINT8(0, drawn.generated);
    const frame_record_choice_t generated = choice_of(1);
    TEST_ASSERT_EQUAL_INT16(2, generated.step);
    TEST_ASSERT_EQUAL_UINT8(50, generated.scale_percent);
    TEST_ASSERT_EQUAL_UINT16(drawn_width, generated.width);
    TEST_ASSERT_EQUAL_UINT16(drawn_height, generated.height);
    TEST_ASSERT_EQUAL_UINT8(1, generated.generated);
}

/* The frame's SETTINGS record is the caller's own, not the copy the tunables override for the draw. */
static void
test_a_settings_record_is_the_callers_own_not_the_tuned_copy(void) {
    knob = tune_find(tune_shared(), TUNE_NAME(render, cull));
    TEST_ASSERT_NOT_NULL(knob);
    open_recording(-1);
    suite_set_test_cleanup(reset_knob);
    render_settings_t own = RENDER_SETTINGS_DEFAULT;
    own.lod.tolerance_px = 3.25F;
    own.lod.pick_every = 3;
    render_context_set_settings(context, &own);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), knob->name, own.cull ? 0 : 1));
    draw_frame();
    TEST_ASSERT_TRUE(context->drawn_settings.cull != own.cull); /* the draw saw the override, the record must not */
    find_records();
    const found_t* record = first_in_frame(0, FRAME_RECORD_SETTINGS);
    TEST_ASSERT_EQUAL_UINT32(sizeof own, record->length);
    TEST_ASSERT_EQUAL_MEMORY(&context->settings, record->body, sizeof own);
    render_settings_t recorded;
    memcpy(&recorded, record->body, sizeof recorded);
    TEST_ASSERT_EQUAL_FLOAT(3.25F, recorded.lod.tolerance_px);
    TEST_ASSERT_EQUAL_INT(3, recorded.lod.pick_every);
    TEST_ASSERT_EQUAL(own.cull, recorded.cull);
}

/* A keyframe's LODSTATE opens with the pick round of the raster and the steering of the context, as they stood
 * before the draw. */
static void
test_a_keyframes_lodstate_carries_the_contexts_steer_and_the_rasters_pick_round(void) {
    open_recording(-1);
    context->steer.tolerance_px = 7.5F;
    context->steer.picked_tolerance_px = 6.25F;
    context->steer.picked_coverage_permille = 123;
    context->steer.bin = 3;
    context->steer.drawn = 99999;
    context->steer.frames = 11;
    context->steer.settled = true;
    context->raster.pick_round = 41;
    const r3d_lod_steer_t before = context->steer;
    draw_frame();
    find_records();
    const found_t* record = first_in_frame(0, FRAME_RECORD_LODSTATE);
    frame_record_lodstate_t state;
    TEST_ASSERT_TRUE(record->length >= sizeof state);
    memcpy(&state, record->body, sizeof state);
    TEST_ASSERT_EQUAL_UINT32(41, state.pick_round);
    TEST_ASSERT_EQUAL_FLOAT(before.tolerance_px, state.steer.tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(before.picked_tolerance_px, state.steer.picked_tolerance_px);
    TEST_ASSERT_EQUAL_INT(before.picked_coverage_permille, state.steer.picked_coverage_permille);
    TEST_ASSERT_EQUAL_INT(before.bin, state.steer.bin);
    TEST_ASSERT_EQUAL_INT64(before.drawn, state.steer.drawn);
    TEST_ASSERT_EQUAL_INT(before.frames, state.steer.frames);
    TEST_ASSERT_TRUE(state.steer.settled);
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

#ifndef DEVICE_BUILD
#define POSITIONS_TYPE ASSET_TYPE('P', 'O', 'S', ' ')

/* Pack `name`: one entry holding the rig's first quad's positions. */
static void
write_positions_pack(const char* name) {
    const r3d_lit_mesh_t* quad = &rig->quad[0].mesh;
    const uint32_t bytes = (uint32_t)(sizeof(*quad->positions) * (size_t)quad->vertex_count);
    const uint32_t capacity = ASSET_PACK_HEADER_SIZE + ASSET_PACK_ENTRY_SIZE + (uint32_t)test_pack_aligned(bytes);
    uint8_t* pack_bytes = malloc(capacity);
    TEST_ASSERT_NOT_NULL(pack_bytes);
    test_pack_t pack = test_pack_begin(pack_bytes, capacity, 1);
    memcpy(test_pack_add(&pack, "pos", POSITIONS_TYPE, bytes), quad->positions, bytes);
    const uint32_t size = test_pack_finish(&pack);
    char path[64];
    (void)snprintf(path, sizeof path, "./%s.apak", name);
    test_write_file(path, pack_bytes, size);
    free(pack_bytes);
}

static void
remove_positions_packs(void) {
    (void)remove("./suite_record_first.apak");
    (void)remove("./suite_record_second.apak");
}

/* The rig's first quad drawn from pack `name`'s positions; the load to release. */
static handle_t
draw_from_pack(const char* name) {
    const handle_t load = asset_store_load(name);
    const asset_pack_t* pack = asset_store_loaded(load);
    TEST_ASSERT_NOT_NULL(pack);
    asset_view_t view;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(pack, "pos", POSITIONS_TYPE, &view));
    rig->quad[0].mesh.positions = (const int16_t (*)[3])view.data;
    draw_frame();
    return load;
}

/* A scene switch between keyframes: the next pack mounts in the slot the
 * last one freed, perhaps at the same address. Every mesh is recorded
 * again, whether its pointers moved or not, and the slot is named anew. */
static void
test_a_pack_mounted_in_a_freed_slot_between_keyframes_is_named_again(void) {
    open_recording(-1);
    const int16_t (*own)[3] = rig->quad[0].mesh.positions;
    test_asset_dir_use(".");
    suite_add_test_cleanup(remove_positions_packs);
    write_positions_pack("suite_record_first");
    write_positions_pack("suite_record_second");
    asset_store_release(draw_from_pack("suite_record_first"));
    const handle_t second = draw_from_pack("suite_record_second");
    rig->quad[0].mesh.positions = own;
    asset_store_release(second);
    find_records();
    TEST_ASSERT_EQUAL_INT(RASTER_RIG_QUADS, count_in_frame(1, FRAME_RECORD_MESH));
    frame_record_pack_t first_pack, second_pack;
    memcpy(&first_pack, first_in_frame(0, FRAME_RECORD_PACK)->body, sizeof first_pack);
    memcpy(&second_pack, first_in_frame(1, FRAME_RECORD_PACK)->body, sizeof second_pack);
    TEST_ASSERT_EQUAL_UINT8(first_pack.slot, second_pack.slot);
    TEST_ASSERT_EQUAL_STRING("suite_record_first", first_pack.name);
    TEST_ASSERT_EQUAL_STRING("suite_record_second", second_pack.name);
}
#endif

void
run_frame_record_suite(void) {
    RUN_TEST(test_a_stream_opens_with_its_header_then_each_frame_with_frame_view_and_choice);
    RUN_TEST(test_keyframes_carry_every_mesh_and_the_frames_between_only_one_that_moved);
    RUN_TEST(test_a_refused_frame_is_dropped_whole_and_counted_and_the_next_is_a_keyframe);
    RUN_TEST(test_a_changed_tunable_rides_in_the_next_frame_and_every_keyframe_while_off_its_initial);
    RUN_TEST(test_a_keyframes_picture_is_hashed_into_the_next_frame_and_the_last_one_at_stop);
    RUN_TEST(test_a_keyframe_without_a_snapshot_drops_instead_of_reusing_lod_state);
    RUN_TEST(test_a_second_start_is_refused_while_recording);
    RUN_TEST(test_a_choice_carries_the_contexts_own_step_scale_size_and_generated_flag);
    RUN_TEST(test_a_settings_record_is_the_callers_own_not_the_tuned_copy);
    RUN_TEST(test_a_keyframes_lodstate_carries_the_contexts_steer_and_the_rasters_pick_round);
    RUN_TEST(test_a_mesh_in_no_pack_is_named_by_no_pack_and_no_pack_is_named);
#ifndef DEVICE_BUILD
    RUN_TEST(test_a_pack_mounted_in_a_freed_slot_between_keyframes_is_named_again);
#endif
    RUN_TEST(test_a_posed_mesh_names_its_bind_and_its_palette_rides_each_keyframe_and_each_new_pose);
    RUN_TEST(test_a_built_mesh_carries_its_source_whole_each_keyframe_and_each_build);
    RUN_TEST(test_a_textured_built_mesh_carries_its_uvs_after_its_triangles);
    RUN_TEST(test_a_generated_frame_on_a_keyframes_turn_hands_the_keyframe_to_the_next_drawn_frame);
    RUN_TEST(test_generated_frames_after_a_drop_wait_for_a_drawn_keyframe);
    RUN_TEST(test_a_hash_whose_frame_is_dropped_rides_the_next_frame_taken);
    RUN_TEST(test_a_mesh_struct_rewritten_in_place_with_new_arrays_is_recorded_again);
    RUN_TEST(test_each_instance_drawing_a_rebuilt_mesh_carries_the_build);
    RUN_TEST(test_a_build_before_a_generated_frame_rides_the_next_drawn_frame);
    RUN_TEST(test_a_full_note_table_gives_its_oldest_note_to_a_new_build);
    RUN_TEST(test_a_mesh_made_in_a_block_forgets_the_notes_of_meshes_that_lived_there);
}

SUITE_REGISTER(run_frame_record_suite);
