/* frame_record against a render context drawing the raster rig: the stream
 * opens with its header, keyframes carry every mesh and the frames between
 * only what moved, a frame the sink refuses is dropped whole and counted
 * with the next one a keyframe, a changed tunable rides in the next frame,
 * and a keyframe's composed picture is hashed into a CHECK. */

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
}

SUITE_REGISTER(run_frame_record_suite);
