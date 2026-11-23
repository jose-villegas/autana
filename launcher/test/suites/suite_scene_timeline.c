#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_file.h"
#include "kinematics/hinge/hinge.h"
#include "kinematics/rig/rig.h"
#include "math/scalar/ease.h"
#include "math/scalar/rng.h"
#include "math/scalar/units.h"

#include "anim/anim_tracks_format.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/scene_asset.h"
#include "scene/scene_internal.h"
#include "scene/timeline/timeline.h"
#include "scene/transform/transform.h"
#include "suites.h"
#include "test_alloc.h"
#if !defined(DEVICE_BUILD)
#include "test_asset_dir.h"
#endif
#include "test_anim_tracks.h"
#include "test_scene_timeline.h"
#include "testkit/cleanup.h"

#include "../scene_examples/timeline.c"

enum {
    CAPACITY = 8192,
    ARENA_BYTES = 4096,
    DURATION = 1000,
    STEP_MAX = 97,
    FIRST_START = 100,
    SECOND_START = 250,
    MID_SPEED_KEY = 500,
    THIRD_START = 625,
    SECOND_BLEND = 500,
    THIRD_BLEND = 250,
    CAMERA = 1,
    SUBJECT = 0,
    BUTTERFLY = 2,
    KEYS = 2,
    TRACKS = 6,
    BLOCK_KEYS = 3,
    SPEED_KEYS = 4,
    ANIMATOR_TRACK_COUNT = TRACKS + BLOCK_KEYS,
    CNST_TRANSFORM_TRACKS = 2,
    CNST_TRACKS = 7,
    CNST_FIRST_CHANGE = 400,
    CNST_LAST_CHANGE = 750,
    TRACK_CAPACITY = TRACKS + CNST_TRACKS,
    RANDOM_SEED = 0x7351U,
    TIMES = TEST_TRACKS_HEADER_SIZE + TRACK_CAPACITY * (TEST_TRACK_ROW_SIZE + TEST_TRACK_STRING_SLOT),
    CAMERA_VALUES = TIMES + KEYS * sizeof(float),
    SUBJECT_VALUES = CAMERA_VALUES + KEYS * 3 * sizeof(float),
    FOV_VALUES = SUBJECT_VALUES + KEYS * 3 * sizeof(float),
    NEAR_VALUES = FOV_VALUES + KEYS * sizeof(float),
    ROTATION_VALUES = NEAR_VALUES + KEYS * sizeof(float),
    SCALE_VALUES = ROTATION_VALUES + KEYS * 4 * sizeof(float),
    BLOCK_TIMES = SCALE_VALUES + KEYS * 3 * sizeof(float),
    BLOCK_NAMES = BLOCK_TIMES + BLOCK_KEYS * sizeof(float),
    BLOCK_BLENDS = BLOCK_NAMES + BLOCK_KEYS * ANIM_NAME_WIDTH * sizeof(float),
    SPEED_TIMES = BLOCK_BLENDS + BLOCK_KEYS * sizeof(float),
    SPEED_VALUES = SPEED_TIMES + SPEED_KEYS * sizeof(float),
    CNST_TIME_BYTES = BLOCK_KEYS * sizeof(float),
    CNST_NAME_BYTES = BLOCK_KEYS * ANIM_NAME_WIDTH * sizeof(float),
    CNST_POSITION = SPEED_VALUES + SPEED_KEYS * sizeof(float),
    CNST_ROTATION = CNST_POSITION + KEYS * 3 * sizeof(float),
    CNST_TIMES = CNST_ROTATION + KEYS * 4 * sizeof(float),
    CNST_FOLLOW = CNST_TIMES + CNST_TIME_BYTES,
    CNST_LOOK = CNST_FOLLOW + CNST_NAME_BYTES,
    CNST_WEIGHT_TIMES = CNST_LOOK + CNST_NAME_BYTES,
    CNST_WEIGHT = CNST_WEIGHT_TIMES + CNST_TIME_BYTES,
    CNST_LAG = CNST_WEIGHT + CNST_TIME_BYTES,
    CNST_HANDOFF = CNST_LAG + CNST_TIME_BYTES,
    CLIP_BYTES = CNST_HANDOFF + CNST_TIME_BYTES
};

static const char CLIP[] = "timeline.clip";
static const char CAMERA_NAME[] = "camera";
static const char POSITION[] = "position";
static const char CAPYBARA_NAME[] = "capybara";
static const char BUTTERFLY_NAME[] = "butterfly";

static void* allocation;
static uint8_t* arena;
static size_t arena_used;
#if !defined(DEVICE_BUILD)
static void* probe_allocation;
#endif

static void*
lend_timeline_memory(size_t bytes, size_t align) {
    size_t end = arena_used;
    const size_t at = mem_carve(&end, bytes, align);
    if (end > ARENA_BYTES) {
        return NULL;
    }
    arena_used = end;
    return arena + at;
}

static void
release_fixture(void) {
    scene_unload_all();
    if (arena != NULL) {
        (void)scene_set_take(NULL);
        memory_free(arena);
        arena = NULL;
        arena_used = 0;
    }
#if !defined(DEVICE_BUILD)
    if (probe_allocation != NULL) {
        asset_file_release(probe_allocation);
        probe_allocation = NULL;
    }
#endif
    test_free_aligned(allocation);
    allocation = NULL;
}

static uint8_t*
part_row(uint8_t* entry, scene_asset_part_t part) {
    return entry + asset_read_u32(entry + SCENE_ASSET_AT_OFFSETS + part * sizeof(uint32_t));
}

static uint8_t*
fixture(test_pack_t* pack, int timelines, int constraints, int hinges, bool property, const char* path,
        const char* field, uint32_t duration) {
    uint8_t* bytes = test_alloc_aligned(CAPACITY, ASSET_PACK_BASE_ALIGN, &allocation);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(bytes);
    const uint16_t counts[SCENE_ASSET_PART_COUNT] = {
        [SCENE_ASSET_TIMELINE] = timelines, [SCENE_ASSET_CONSTRAINTS] = constraints, [SCENE_ASSET_HINGES] = hinges};
    const size_t sizes[SCENE_ASSET_PART_COUNT] = {[SCENE_ASSET_TIMELINE] = sizeof(scene_asset_timeline_t),
                                                  [SCENE_ASSET_CONSTRAINTS] = sizeof(scene_asset_constraint_t),
                                                  [SCENE_ASSET_HINGES] = sizeof(scene_asset_hinge_t)};
    uint8_t* entry = scene_test_timeline_entry(pack, bytes, CAPACITY, counts, sizes, property);
    uint8_t* clip = test_pack_add(pack, CLIP, ANIM_TRACKS_ASSET, CLIP_BYTES);
    test_tracks_header(clip, TRACKS, duration);
    const test_track_t tracks[] = {
        {path, field, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, CAMERA_VALUES, KEYS, ANIM_LINEAR},
        {constraints ? CAPYBARA_NAME : "red", POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES,
         SUBJECT_VALUES, KEYS, ANIM_LINEAR},
        {CAMERA_NAME, "half_fov_short_tan", ANIM_COMPONENT_CAMERA, ANIM_VALUE_FLOAT, TIMES, FOV_VALUES, KEYS,
         ANIM_LINEAR},
        {CAMERA_NAME, "near_z", ANIM_COMPONENT_CAMERA, ANIM_VALUE_FLOAT, TIMES, NEAR_VALUES, KEYS, ANIM_LINEAR},
        {CAMERA_NAME, "rotation", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, ROTATION_VALUES, KEYS, ANIM_LINEAR},
        {constraints ? CAPYBARA_NAME : "red", "scale", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SCALE_VALUES,
         KEYS, ANIM_LINEAR},
    };
    const float times[] = {0.0F, 1.0F};
    const float values[] = {3,    0, -10, 6, 0, -10, 1, 0, 0, 4, 0, 0, 0.5F, 0.75F, 0.25F,
                            0.5F, 0, 0,   0, 1, 0,   0, 1, 0, 1, 1, 1, 2,    2,     2};
    for (int i = 0; i < TRACKS; i++) {
        test_track_row(clip, i, &tracks[i]);
    }
    test_pack_put_floats(clip + TIMES, times, KEYS);
    test_pack_put_floats(clip + CAMERA_VALUES, values, sizeof values / sizeof *values);
    if (timelines) {
        scene_asset_timeline_t row = {.wrap = ANIM_CLAMP};
        memcpy(row.clip, CLIP, sizeof CLIP);
        memcpy(part_row(entry, SCENE_ASSET_TIMELINE), &row, sizeof row);
    }
    if (constraints) {
        const scene_asset_constraint_t row = {.entity = CAMERA, .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    }
    if (hinges) {
        const scene_asset_hinge_t row = {.entity = CAMERA, .axis = {0, 0, 1}, .min = -1.0F, .max = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_HINGES), &row, sizeof row);
    }
    return entry;
}

static entity_t
load(asset_pack_t* pack) {
    return scene_load_from(pack, SCENE_TEST_TIMELINE_ID);
}

static scene_t*
columns(entity_t root) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(root, &row);
    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_NOT_NULL(record->scene);
    return record->scene;
}

static void
open_pack(test_pack_t* writer, asset_pack_t* pack) {
    const uint32_t bytes = test_pack_finish(writer);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(pack, writer->bytes, bytes));
}

static void
put_blocks(test_pack_t* writer, const char* last_clip) {
    uint8_t* clip = writer->bytes + writer->end - test_pack_aligned(CLIP_BYTES);
    const test_track_t tracks[] = {
        {"red", "clip", ANIM_COMPONENT_ANIMATOR, ANIM_VALUE_NAME, BLOCK_TIMES, BLOCK_NAMES, BLOCK_KEYS, ANIM_STEP},
        {"red", "speed", ANIM_COMPONENT_ANIMATOR, ANIM_VALUE_FLOAT, SPEED_TIMES, SPEED_VALUES, SPEED_KEYS, ANIM_STEP},
        {"red", "blend_ms", ANIM_COMPONENT_ANIMATOR, ANIM_VALUE_FLOAT, BLOCK_TIMES, BLOCK_BLENDS, BLOCK_KEYS,
         ANIM_STEP},
    };
    const size_t strings = TEST_TRACKS_HEADER_SIZE + ANIMATOR_TRACK_COUNT * TEST_TRACK_ROW_SIZE;
    const size_t old_strings = TEST_TRACKS_HEADER_SIZE + TRACKS * TEST_TRACK_ROW_SIZE;
    memmove(clip + strings, clip + old_strings, TRACKS * TEST_TRACK_STRING_SLOT);
    test_tracks_header(clip, ANIMATOR_TRACK_COUNT, DURATION);
    const char* const names[] = {"idle", "walk", last_clip};
    const float times[] = {(float)FIRST_START / MATH_MS_PER_S, (float)SECOND_START / MATH_MS_PER_S,
                           (float)THIRD_START / MATH_MS_PER_S};
    const float speed_times[] = {times[0], times[1], (float)MID_SPEED_KEY / MATH_MS_PER_S, times[2]};
    const float speeds[] = {1.0F, 0.3F, 1.0F / 3.0F, 1.25F};
    const float blends[] = {0, SECOND_BLEND, THIRD_BLEND};
    float values[BLOCK_KEYS * ANIM_NAME_WIDTH];
    for (int i = 0; i < BLOCK_KEYS; i++) {
        anim_name_encode(name_hash(names[i]), values + i * ANIM_NAME_WIDTH);
        test_track_row(clip, TRACKS + i, &tracks[i]);
    }
    test_pack_put_floats(clip + BLOCK_TIMES, times, BLOCK_KEYS);
    test_pack_put_floats(clip + BLOCK_NAMES, values, BLOCK_KEYS * ANIM_NAME_WIDTH);
    test_pack_put_floats(clip + BLOCK_BLENDS, blends, BLOCK_KEYS);
    test_pack_put_floats(clip + SPEED_TIMES, speed_times, SPEED_KEYS);
    test_pack_put_floats(clip + SPEED_VALUES, speeds, SPEED_KEYS);
}

static void
test_animator_track_load_errors_name_the_field(void) {
    const struct {
        const char* last_clip;
        const char* path;
        const char* field;
        float value;
        bool remove_clip;
    } cases[] = {
        {"missing", "red", "clip", 0, false},
        {"costarring", "red", "clip", 0, false},
        {"gallop", CAMERA_NAME, "clip", 0, false},
        {"gallop", "red", "speed", 0, true},
        {"gallop", "red", "blend_ms", 0, true},
        {"gallop", "red", "speed", -1, false},
        {"gallop", "red", "speed", NAN, false},
        {"gallop", "red", "speed", (float)ANIM_PLAYHEAD_SPEED_MAX + 1.0F, false},
        {"gallop", "red", "blend_ms", -1, false},
        {"gallop", "red", "blend_ms", (float)UINT32_MAX, false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        put_blocks(&writer, cases[i].last_clip);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        if (strcmp(cases[i].last_clip, "costarring") == 0) {
            uint8_t* names = part_row(entry, SCENE_ASSET_CLIPS);
            memset(names, 0, 2 * ASSET_NAME_MAX);
            memcpy(names, "costarring", sizeof "costarring");
            memcpy(names + ASSET_NAME_MAX, "liquid", sizeof "liquid");
            float value[ANIM_NAME_WIDTH];
            anim_name_encode(name_hash(cases[i].last_clip), value);
            test_pack_put_floats(clip + BLOCK_NAMES, value, ANIM_NAME_WIDTH);
            test_pack_put_floats(clip + BLOCK_NAMES + ANIM_NAME_WIDTH * sizeof(float), value, ANIM_NAME_WIDTH);
        }
        if (strcmp(cases[i].path, CAMERA_NAME) == 0) {
            const test_track_t wrong = {CAMERA_NAME,     "clip",      ANIM_COMPONENT_ANIMATOR,
                                        ANIM_VALUE_NAME, BLOCK_TIMES, BLOCK_NAMES,
                                        BLOCK_KEYS,      ANIM_STEP};
            test_track_row(clip, TRACKS, &wrong);
        } else if (strcmp(cases[i].field, "clip") != 0) {
            const bool speed = strcmp(cases[i].field, "speed") == 0;
            const test_track_t control = {"red",
                                          cases[i].field,
                                          ANIM_COMPONENT_ANIMATOR,
                                          ANIM_VALUE_FLOAT,
                                          speed ? SPEED_TIMES : BLOCK_TIMES,
                                          speed ? SPEED_VALUES : BLOCK_BLENDS,
                                          speed ? SPEED_KEYS : BLOCK_KEYS,
                                          ANIM_STEP};
            if (cases[i].remove_clip) {
                const size_t strings = TEST_TRACKS_HEADER_SIZE + (TRACKS + 1) * TEST_TRACK_ROW_SIZE;
                const size_t old_strings = TEST_TRACKS_HEADER_SIZE + ANIMATOR_TRACK_COUNT * TEST_TRACK_ROW_SIZE;
                memmove(clip + strings, clip + old_strings, TRACKS * TEST_TRACK_STRING_SLOT);
                test_tracks_header(clip, TRACKS + 1, DURATION);
                test_track_row(clip, TRACKS, &control);
            }
            test_pack_put_floats(clip + (speed ? SPEED_VALUES : BLOCK_BLENDS), &cases[i].value, 1);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
        TEST_ASSERT_NOT_NULL(strstr(failure.what, cases[i].path));
        TEST_ASSERT_NOT_NULL(strstr(failure.what, cases[i].field));
        release_fixture();
    }
}

static void
test_rejected_non_animator_row_names_the_binding(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const float invalid = NAN;
    test_pack_put_floats(clip + NEAR_VALUES, &invalid, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
    TEST_ASSERT_EQUAL_STRING("camera:CAMR.near_z", failure.what);
}

static void
test_timeline_validates_rounded_block_times(void) {
    static const struct {
        uint32_t duration;
        size_t offset;
        float time;
        const char* failed;
    } cases[] = {
        {1500, BLOCK_TIMES + sizeof(float), 0.6246F, "red:ANMR.clip"},
        {1500, SPEED_TIMES + 2 * sizeof(float), 0.6246F, "red:ANMR.speed"},
        {THIRD_START, BLOCK_TIMES, 0.1F, "red:ANMR.clip"},
        {DURATION, BLOCK_TIMES + 2 * sizeof(float), 0.9F, "red:ANMR.blend_ms"},
        {16777217, BLOCK_TIMES + 2 * sizeof(float), 16777.216F, NULL},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        put_blocks(&writer, "gallop");
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        test_pack_put32(clip + ANIM_TRACKS_AT_DURATION, cases[i].duration);
        test_pack_put_floats(clip + cases[i].offset, &cases[i].time, 1);
        if (cases[i].failed == NULL) {
            const float cut = 0;
            test_pack_put_floats(clip + BLOCK_BLENDS + 2 * sizeof(float), &cut, 1);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        if (cases[i].failed != NULL) {
            scene_failure_t failure;
            TEST_ASSERT_TRUE_MESSAGE(scene_failure(root, &failure), cases[i].failed);
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
            TEST_ASSERT_EQUAL_STRING(cases[i].failed, failure.what);
        } else {
            timeline_seek(root, cases[i].duration - 1);
            TEST_ASSERT_EQUAL_UINT8(2, columns(root)->animators[SUBJECT].layer.current.clip);
        }
        release_fixture();
    }
}

static void
assert_timeline_pose(const scene_t* a, const scene_t* b, bool palette_due, const char* message) {
    for (int e = 0; e < a->asset.entity_count; e++) {
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&a->locals[e].position, &b->locals[e].position, sizeof(vec3f_t), message);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&a->locals[e].rotation, &b->locals[e].rotation, sizeof(quatf_t), message);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&a->locals[e].scale, &b->locals[e].scale, sizeof(vec3f_t), message);
    }
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&a->cameras[0].lens, &b->cameras[0].lens, sizeof a->cameras[0].lens, message);
    const scene_animator_t* x = &a->animators[SUBJECT];
    const scene_animator_t* y = &b->animators[SUBJECT];
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&x->layer.current, &y->layer.current, sizeof x->layer.current, message);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&x->layer.previous, &y->layer.previous, sizeof x->layer.previous, message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->layer.blend_ms, y->layer.blend_ms, message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(x->layer.blended_ms, y->layer.blended_ms, message);
    TEST_ASSERT_FALSE_MESSAGE(y->layer.frozen_previous, message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, x->owed_ms, message);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, y->owed_ms, message);
    TEST_ASSERT_FALSE_MESSAGE(y->timeline_pose_pending, message);
    const scene_skinned_renderer_t* skin_a = &a->skinned_renderers[SUBJECT];
    const scene_skinned_renderer_t* skin_b = &b->skinned_renderers[SUBJECT];
    TEST_ASSERT_NOT_NULL_MESSAGE(skin_a->palette, message);
    TEST_ASSERT_NOT_NULL_MESSAGE(skin_b->palette, message);
    if (palette_due) {
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(skin_a->palette, skin_b->palette,
                                         skin_a->skin.joint_count * sizeof *skin_a->palette, message);
    }
}

static void
test_seek_matches_play_at_the_same_millisecond(void) {
    const uint32_t edges[] = {FIRST_START,
                              SECOND_START,
                              MID_SPEED_KEY,
                              THIRD_START,
                              SECOND_START + SECOND_BLEND,
                              THIRD_START + THIRD_BLEND,
                              DURATION,
                              DURATION + FIRST_START};
    rng_t random;
    rng_seed(&random, RANDOM_SEED);
    const anim_wrap_t wraps[] = {ANIM_CLAMP, ANIM_LOOP};
    for (size_t mode = 0; mode < sizeof wraps / sizeof *wraps; mode++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[offsetof(scene_asset_timeline_t, wrap)] = wraps[mode];
        put_blocks(&writer, "gallop");
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t played = load(&pack), sought = load(&pack);
        scene_t* a = columns(played);
        scene_t* b = columns(sought);
        timeline_play(played);
        uint32_t elapsed = 0;
        bool posed = false, skipped = false, blended = false;
        for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) {
            for (uint32_t target = edges[i] - 1; target <= edges[i] + 1; target++) {
                for (; elapsed < target;) {
                    const uint32_t draw = (uint32_t)rng_below(&random, STEP_MAX) + 1;
                    const uint32_t dt = draw < target - elapsed ? draw : target - elapsed;
                    scene_animation_step(a, dt);
                    elapsed += dt;
                }
                timeline_seek(sought, target);
                const uint32_t expected =
                    wraps[mode] == ANIM_LOOP ? target % DURATION : (target < DURATION ? target : DURATION);
                char message[80];
                (void)snprintf(message, sizeof message, "%s at %lu ms (edge %lu ms)",
                               wraps[mode] == ANIM_LOOP ? "loop" : "clamp", (unsigned long)target,
                               (unsigned long)edges[i]);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, timeline_time_ms(played), message);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected, timeline_time_ms(sought), message);
                const scene_animator_t* anim = &a->animators[SUBJECT];
                const bool due = scene_animator_due(a->animation_frame - 1, anim);
                TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(1, anim->interval, message);
                assert_timeline_pose(a, b, due, message);
                posed |= due;
                skipped |= !due;
                blended |= anim->layer.blended_ms < anim->layer.blend_ms;
                if (expected < FIRST_START) {
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ANIM_LAYER_CLIP_NONE, anim->layer.current.clip, message);
                    const vec3f_t rest_position = vec3f_zero();
                    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&rest_position,
                                                     &b->animators[SUBJECT].layer.shown.local[0].position,
                                                     sizeof rest_position, message);
                } else {
                    const int active = expected < SECOND_START ? 0 : expected < THIRD_START ? 1 : 2;
                    TEST_ASSERT_EQUAL_INT_MESSAGE(active, anim->layer.current.clip, message);
                }
            }
        }
        TEST_ASSERT_TRUE(posed && skipped && blended);
        const uint32_t rewind = SECOND_START + 1;
        const entity_t fresh = load(&pack);
        timeline_seek(played, rewind);
        timeline_seek(fresh, rewind);
        assert_timeline_pose(a, columns(fresh), true, wraps[mode] == ANIM_LOOP ? "loop rewind" : "clamp rewind");
        TEST_ASSERT_FALSE(a->animators[SUBJECT].timeline_pose_pending);
        TEST_ASSERT_EQUAL_UINT32(rewind, timeline_time_ms(played));

        const entity_t driven = scene_row_entity(a, SUBJECT);
        const char* const clips[] = {"idle", "walk", "gallop"};
        TEST_ASSERT_FALSE(animator_play(driven, 1, SECOND_BLEND));
        TEST_ASSERT_FALSE(animator_set_clips(driven, "rig", clips, sizeof clips / sizeof *clips, NULL));
        const animator_slot_t slots[] = {ANIMATOR_CURRENT, ANIMATOR_PREVIOUS};
        for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) {
            TEST_ASSERT_FALSE(animator_set_speed(driven, slots[i], 0.5F));
            TEST_ASSERT_FALSE(animator_set_paused(driven, slots[i], true));
            TEST_ASSERT_FALSE(animator_seek(driven, slots[i], rewind));
        }
        release_fixture();
        fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        open_pack(&writer, &pack);
        const entity_t manual = scene_row_entity(columns(load(&pack)), SUBJECT);
        TEST_ASSERT_TRUE(animator_set_clips(manual, "rig", clips, sizeof clips / sizeof *clips, NULL));
        TEST_ASSERT_TRUE(animator_play(manual, 0, 0));
        TEST_ASSERT_TRUE(animator_play(manual, 1, SECOND_BLEND));
        for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) {
            TEST_ASSERT_TRUE(animator_set_speed(manual, slots[i], 0.5F));
            TEST_ASSERT_TRUE(animator_set_paused(manual, slots[i], true));
            TEST_ASSERT_TRUE(animator_seek(manual, slots[i], rewind));
        }
        release_fixture();
    }
}

static void
test_clock_and_immediate_seek(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_FALSE(timeline_playing(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_UINT32(DURATION, timeline_duration_ms(root));
    TEST_ASSERT_EQUAL_FLOAT(3.0F, scene->locals[CAMERA].position.x);
    timeline_play(root);
    scene_animation_step(scene, DURATION / 2);
    TEST_ASSERT_EQUAL_UINT32(DURATION / 2, timeline_time_ms(root));
    timeline_pause(root);
    scene_animation_step(scene, DURATION);
    TEST_ASSERT_EQUAL_UINT32(DURATION / 2, timeline_time_ms(root));
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(scene, CAMERA)));
    render_context_t* render = render_context_main();
    render->kept = true;
    scene_set_paused(true);
    timeline_seek(root, UINT32_MAX);
    TEST_ASSERT_FALSE(render->kept);
    TEST_ASSERT_EQUAL_UINT32(DURATION, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_FLOAT(6.0F, scene->locals[CAMERA].position.x);
    TEST_ASSERT_NOT_NULL(scene->skinned_renderers[SUBJECT].palette);
    scene_set_paused(false);
    timeline_play(root);
    scene_animation_step(scene, UINT32_MAX);
    TEST_ASSERT_TRUE(timeline_playing(root));
    TEST_ASSERT_EQUAL_UINT32(DURATION, timeline_time_ms(root));
}

static void
test_zero_duration_loop_holds_zero(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, 0);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    timeline_play(root);
    timeline_seek(root, UINT32_MAX);
    scene_animation_step(columns(root), UINT32_MAX);
    TEST_ASSERT_TRUE(timeline_playing(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_FLOAT(3.0F, columns(root)->locals[CAMERA].position.x);
}

static void
test_scene_without_a_timeline_ignores_controls(void) {
    test_pack_t writer;
    fixture(&writer, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const transformf_t before = scene->locals[CAMERA];
    timeline_play(root);
    timeline_seek(root, DURATION);
    TEST_ASSERT_FALSE(timeline_playing(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_duration_ms(root));
    TEST_ASSERT_EQUAL_MEMORY(&before, &scene->locals[CAMERA], sizeof before);
    timeline_pause(root);
}

static void
test_load_names_the_failing_clip_field_or_table(void) {
    static const struct {
        const char* path;
        const char* field;
        const char* what;
        bool missing, property, hinge;
    } cases[] = {
        {CAMERA_NAME, POSITION, "missing", .missing = true},
        {.path = "missing", .field = POSITION, .what = "missing:TRNS.position"},
        {.path = CAMERA_NAME, .field = "missing", .what = "camera:TRNS.missing"},
        {.path = ANIM_TARGET_SELF, .field = POSITION, .what = ANIM_TARGET_SELF ":TRNS.position"},
        {CAMERA_NAME, POSITION, ANIM_TARGET_SELF ":TRNS.position", .property = true},
        {CAMERA_NAME, POSITION, "camera:TRNS.position", .hinge = true},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry =
            fixture(&writer, 1, 0, cases[i].hinge, cases[i].property, cases[i].path, cases[i].field, DURATION);
        if (cases[i].missing) {
            memset(part_row(entry, SCENE_ASSET_TIMELINE), 0, ASSET_NAME_MAX);
            memcpy(part_row(entry, SCENE_ASSET_TIMELINE), cases[i].what, strlen(cases[i].what));
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(cases[i].missing ? ASSET_ERR_NOT_FOUND : ASSET_ERR_FORMAT, failure.asset);
        TEST_ASSERT_EQUAL_STRING(cases[i].what, failure.what);
        release_fixture();
    }
}

static void
test_scne_validates_every_new_table(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 2, 2, 2, false, CAMERA_NAME, POSITION, DURATION);
    const asset_view_t view = {.data = entry,
                               .size = (uint32_t)(part_row(entry, SCENE_ASSET_HINGES) - entry)
                                       + 2 * sizeof(scene_asset_hinge_t)};
    scene_asset_t asset;
    const scene_asset_part_t parts[] = {SCENE_ASSET_TIMELINE, SCENE_ASSET_CONSTRAINTS, SCENE_ASSET_HINGES};
    const size_t strides[] = {sizeof(scene_asset_timeline_t), sizeof(scene_asset_constraint_t),
                              sizeof(scene_asset_hinge_t)};
    for (int i = 0; i < 3; i++) {
        uint8_t* row = part_row(entry, parts[i]);
        memcpy(row + strides[i], row, strides[i]);
        uint8_t* count = entry + SCENE_ASSET_AT_COUNTS + parts[i] * sizeof(uint16_t);
        test_pack_put16(count, 1);
    }
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(view, &asset));

    const asset_view_t incomplete_header = {.data = entry, .size = SCENE_ASSET_HEADER_SIZE - 1};
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, scene_asset_open(incomplete_header, &asset));
    uint8_t* constraint = part_row(entry, SCENE_ASSET_CONSTRAINTS);
    const float zero = 0.0F;
    test_pack_put_floats(constraint + SCENE_ASSET_CONSTRAINT_AT_WEIGHT, &zero, 1);
    uint8_t* hinge = part_row(entry, SCENE_ASSET_HINGES);
    test_pack_put_floats(hinge + SCENE_ASSET_HINGE_AT_MIN, &zero, 1);
    test_pack_put_floats(hinge + SCENE_ASSET_HINGE_AT_MAX, &zero, 1);
    const float inside[] = {1.00004F, 0, 0};
    test_pack_put_floats(hinge + SCENE_ASSET_HINGE_AT_AXIS, inside, 3);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(view, &asset));
    const float outside[] = {0.9999856352806091F, 0.011344963684678078F, 0};
    test_pack_put_floats(hinge + SCENE_ASSET_HINGE_AT_AXIS, outside, 3);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
    const float unit[] = {0, 0, 1};
    test_pack_put_floats(hinge + SCENE_ASSET_HINGE_AT_AXIS, unit, 3);

    enum { PREVIOUS_VERSION = 6, BAD_CODE = UINT8_MAX };

    test_pack_put16(entry, PREVIOUS_VERSION);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, scene_asset_open(view, &asset));
    test_pack_put16(entry, SCENE_ASSET_VERSION);
    for (int i = 0; i < 3; i++) {
        uint8_t* count = entry + SCENE_ASSET_AT_COUNTS + parts[i] * sizeof(uint16_t);
        test_pack_put16(count, 2);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        test_pack_put16(count, 1);
    }
    uint8_t* timeline = part_row(entry, SCENE_ASSET_TIMELINE);
    uint8_t* constraints = part_row(entry, SCENE_ASSET_CONSTRAINTS);
    uint8_t* hinges = part_row(entry, SCENE_ASSET_HINGES);

    const struct {
        uint8_t* byte;
        uint8_t bad;
    } bytes[] = {
        {timeline + SCENE_ASSET_TIMELINE_AT_CLIP, 0},
        {timeline + SCENE_ASSET_TIMELINE_AT_WRAP, BAD_CODE},
        {timeline + SCENE_ASSET_TIMELINE_AT_PAD, 1},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_ENTITY, BAD_CODE},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_BIND, BAD_CODE},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_PAD, 1},
        {hinges + SCENE_ASSET_HINGE_AT_ENTITY, BAD_CODE},
        {hinges + SCENE_ASSET_HINGE_AT_PAD, 1},
    };

    for (size_t i = 0; i < sizeof bytes / sizeof *bytes; i++) {
        const uint8_t good = *bytes[i].byte;
        *bytes[i].byte = bytes[i].bad;
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        *bytes[i].byte = good;
    }

    const struct {
        uint8_t* at;
        float bad;
    } floats[] = {
        {constraints + SCENE_ASSET_CONSTRAINT_AT_OFFSET, NAN},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_LOOK_OFFSET, INFINITY},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_LAG_MS, -1.0F},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_LAG_MS, INFINITY},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_LAG_MS, (float)ANIM_TRACKS_DURATION_MAX},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_HANDOFF_MS, (float)ANIM_TRACKS_DURATION_MAX},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_HANDOFF_MS, -1.0F},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_HANDOFF_MS, NAN},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_WEIGHT, -0.5F},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_WEIGHT, 1.5F},
        {constraints + SCENE_ASSET_CONSTRAINT_AT_WEIGHT, NAN},
        {hinges + SCENE_ASSET_HINGE_AT_AXIS, 1.0F},
        {hinges + SCENE_ASSET_HINGE_AT_AXIS, NAN},
        {hinges + SCENE_ASSET_HINGE_AT_MIN, 2.0F},
        {hinges + SCENE_ASSET_HINGE_AT_MIN, -INFINITY},
        {hinges + SCENE_ASSET_HINGE_AT_MAX, INFINITY},
        {hinges + SCENE_ASSET_HINGE_AT_ANGLE, NAN},
    };

    for (size_t i = 0; i < sizeof floats / sizeof *floats; i++) {
        float good;
        memcpy(&good, floats[i].at, sizeof good);
        test_pack_put_floats(floats[i].at, &floats[i].bad, 1);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        test_pack_put_floats(floats[i].at, &good, 1);
    }
}

static void
test_timeline_leaves_resting_transforms_clean(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const uint32_t resting[] = {0, DURATION};
    for (size_t i = 0; i < sizeof resting / sizeof *resting; i++) {
        timeline_seek(root, resting[i]);
        scene->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_CHANGED;
        scene->locals[CAMERA].cached = true;
        scene_timeline_step(scene, DURATION);
        TEST_ASSERT_EQUAL_UINT8(0, scene->flags[CAMERA] & SCENE_FLAG_CHANGED);
        TEST_ASSERT_TRUE(scene->locals[CAMERA].cached);
        timeline_play(root);
    }
}

static void
test_runtime_animator_cannot_take_timeline_field(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const char* clips[] = {CLIP};
    scene_failure_t failure;
    TEST_ASSERT_FALSE(animator_set_clips(scene_row_entity(scene, CAMERA), NULL, clips, 1, &failure));
    TEST_ASSERT_EQUAL_STRING("camera:TRNS.position", failure.what);
    timeline_seek(root, DURATION / 2);
    const transformf_t at_middle = scene->locals[CAMERA];
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_EQUAL_MEMORY(&at_middle, &scene->locals[CAMERA], sizeof at_middle);
}

static void
test_camera_cut_on_loop_wrap(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(scene, CAMERA)));
    render_context_t* render = render_context_main();
    render->kept = true;
    timeline_play(root);
    scene_timeline_step(scene, DURATION - 1);
    TEST_ASSERT_TRUE(render->kept);
    scene_timeline_step(scene, 1);
    TEST_ASSERT_FALSE(render->kept);
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    render->kept = true;
    scene_timeline_step(scene, 2 * DURATION);
    TEST_ASSERT_FALSE(render->kept);
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    const entity_t other = scene_load_from(&pack, SCENE_TEST_STATIC_ID);
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(columns(other), CAMERA)));
    render->kept = true;
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_TRUE(render->kept);
    scene_timeline_step(scene, DURATION);
    TEST_ASSERT_TRUE(render->kept);
}

static void
test_timeline_seek_clears_fraction(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    anim_playhead_t* clock = &columns(root)->timeline->clock;
    TEST_ASSERT_TRUE(anim_playhead_set_speed(clock, 0.5F));
    timeline_play(root);
    scene_timeline_step(columns(root), 1);
    TEST_ASSERT_NOT_EQUAL(0, clock->fraction);
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_EQUAL_UINT16(0, clock->fraction);
}

static void
test_scene_rejects_overlapping_tables(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    const asset_view_t view = {.data = entry, .size = (uint32_t)(part_row(entry, SCENE_ASSET_HINGES) - entry)};

    enum { TABLE = SCENE_ASSET_AT_OFFSETS + SCENE_ASSET_SETTINGS * sizeof(uint32_t) };

    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(view, &asset));
    const uint32_t overlap = (uint32_t)(part_row(entry, SCENE_ASSET_CONSTRAINTS) - entry);
    test_pack_put32(entry + TABLE, overlap);
    TEST_ASSERT_NOT_EQUAL(ASSET_OK, scene_asset_open(view, &asset));
}

static void
test_timeline_refuses_duplicate_tracks(void) {
    const test_track_t duplicate = {
        CAMERA_NAME, POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, CAMERA_VALUES, KEYS, ANIM_LINEAR};
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_track_row(clip, 1, &duplicate);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_STRING("camera:TRNS.position", failure.what);
}

static void
test_scene_writers_refuse_bad_lens_keys(void) {
    for (int timeline = 0; timeline <= 1; timeline++) {
        test_pack_t writer;
        fixture(&writer, timeline, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const float invalid = 0.0F;
        test_pack_put_floats(clip + NEAR_VALUES + sizeof(float), &invalid, 1);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        scene_failure_t failure;
        if (timeline) {
            TEST_ASSERT_TRUE(scene_failure(root, &failure));
        } else {
            const char* clips[] = {CLIP};
            const entity_t camera = scene_row_entity(columns(root), CAMERA);
            TEST_ASSERT_FALSE(animator_set_clips(camera, NULL, clips, 1, &failure));
        }
        TEST_ASSERT_EQUAL_STRING("camera:CAMR.near_z", failure.what);
        release_fixture();
    }
}

static void
test_hinge_holds_default_angle(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 0, 1, false, CAMERA_NAME, POSITION, DURATION);
    const float angle = 0.5F;
    test_pack_put_floats(part_row(entry, SCENE_ASSET_HINGES) + SCENE_ASSET_HINGE_AT_ANGLE, &angle, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const quatf_t expected = quatf_from_axis_angle(vec3f_unit_z(), angle);
    for (int i = 0; i < 2; i++) {
        const quatf_t actual = columns(root)->locals[CAMERA].rotation;
        TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof actual);
        scene_animation_step(columns(root), STEP_MAX);
    }
}

static void
test_hinge_seek_equals_play(void) {
    enum { WING = BUTTERFLY + 1, FLAP_TRACKS = 3, FLAP_VALUE_BYTES = KEYS * sizeof(float), FLAP_SAMPLE = DURATION / 2 };

    const uint32_t hinge_component = ASSET_TYPE('H', 'N', 'G', 'E');
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 2, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_hinge_t wings[] = {
        {.entity = WING, .axis = {0, 0, 1}, .min = -0.5F, .max = 0.5F},
        {.entity = BUTTERFLY, .axis = {0, 0, 1}, .min = -0.5F, .max = 0.5F},
    };
    memcpy(part_row(entry, SCENE_ASSET_HINGES), wings, sizeof wings);
    for (int row = BUTTERFLY; row <= WING; row++) {
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + row * sizeof(uint16_t), SUBJECT);
    }
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_tracks_header(clip, FLAP_TRACKS, DURATION);
    const test_track_t tracks[] = {
        {"red", POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SUBJECT_VALUES, KEYS, ANIM_LINEAR},
        {"wing", "angle", hinge_component, ANIM_VALUE_FLOAT, TIMES, CNST_WEIGHT_TIMES, KEYS, ANIM_LINEAR},
        {BUTTERFLY_NAME, "angle", hinge_component, ANIM_VALUE_FLOAT, TIMES, CNST_WEIGHT_TIMES + FLAP_VALUE_BYTES, KEYS,
         ANIM_LINEAR},
    };
    for (int i = 0; i < FLAP_TRACKS; i++) {
        test_track_row(clip, i, &tracks[i]);
    }
    const float angles[] = {0, 2.0F, 0, -2.0F};
    test_pack_put_floats(clip + CNST_WEIGHT_TIMES, angles, sizeof angles / sizeof *angles);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack);
    const entity_t sought = load(&pack);
    timeline_play(played);
    for (uint32_t elapsed = 0; elapsed < DURATION + FLAP_SAMPLE; elapsed += FLAP_SAMPLE) {
        scene_animation_step(columns(played), FLAP_SAMPLE);
    }
    timeline_seek(sought, FLAP_SAMPLE);
    TEST_ASSERT_EQUAL_UINT32(FLAP_SAMPLE, timeline_time_ms(played));
    for (int row = SUBJECT; row <= WING; row++) {
        if (row == CAMERA) {
            continue;
        }
        const transformf_t a = scene_row_world(columns(played)->root, row);
        const transformf_t b = scene_row_world(columns(sought)->root, row);
        TEST_ASSERT_EQUAL_MEMORY(&a.position, &b.position, sizeof a.position);
        TEST_ASSERT_EQUAL_MEMORY(&a.rotation, &b.rotation, sizeof a.rotation);
        if (row != SUBJECT) {
            const float limit = row == WING ? wings[0].max : wings[1].min;
            const quatf_t expected = quatf_from_axis_angle(vec3f_unit_z(), limit);
            const quatf_t actual = columns(sought)->locals[row].rotation;
            TEST_ASSERT_EQUAL_FLOAT(expected.x, actual.x);
            TEST_ASSERT_EQUAL_FLOAT(expected.y, actual.y);
            TEST_ASSERT_EQUAL_FLOAT(expected.z, actual.z);
            TEST_ASSERT_EQUAL_FLOAT(expected.w, actual.w);
        }
    }
}

static void
test_hinge_refuses_transform_writers(void) {
    for (int property = 0; property <= 1; property++) {
        for (int rotation = 0; rotation <= 1; rotation++) {
            test_pack_t writer;
            uint8_t* entry = fixture(&writer, !property, 0, 1, property, CAMERA_NAME, POSITION, DURATION);
            if (rotation) {
                if (property) {
                    memcpy(part_row(entry, SCENE_ASSET_CLIPS), CLIP, sizeof CLIP);
                }
                uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
                test_tracks_header(clip, 1, DURATION);
                const test_track_t track = {property ? ANIM_TARGET_SELF : CAMERA_NAME,
                                            "rotation",
                                            ANIM_COMPONENT_TRANSFORM,
                                            ANIM_VALUE_QUAT,
                                            TIMES,
                                            ROTATION_VALUES,
                                            KEYS,
                                            ANIM_LINEAR};
                test_track_row(clip, 0, &track);
            }
            asset_pack_t pack;
            open_pack(&writer, &pack);
            scene_failure_t failure;
            TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
            TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
            const char* expected[][2] = {{"camera:TRNS.position", "camera:TRNS.rotation"},
                                         {ANIM_TARGET_SELF ":TRNS.position", ANIM_TARGET_SELF ":TRNS.rotation"}};
            TEST_ASSERT_EQUAL_STRING(expected[property][rotation], failure.what);
            release_fixture();
        }
    }
}

enum {
    VIS_FIRST = DURATION / 4,
    VIS_DARK = 3 * DURATION / 8,
    VIS_SECOND = DURATION / 2,
    VIS_RELIT = 5 * DURATION / 8,
    VIS_THIRD = 3 * DURATION / 4,
};

static void
enabled_fixture(test_pack_t* pack, anim_interp_t interp) {
    const size_t capacity = 2 * CAPACITY;
    uint8_t* bytes = test_alloc_aligned(capacity, ASSET_PACK_BASE_ALIGN, &allocation);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(bytes);
    const uint16_t counts[SCENE_ASSET_PART_COUNT] = {[SCENE_ASSET_TIMELINE] = 1, [SCENE_ASSET_LIGHTS] = 1};
    const size_t sizes[SCENE_ASSET_PART_COUNT] = {[SCENE_ASSET_TIMELINE] = sizeof(scene_asset_timeline_t)};
    uint8_t* entry = scene_test_timeline_entry(pack, bytes, capacity, counts, sizes, false);
    test_pack_put16(part_row(entry, SCENE_ASSET_LIGHTS) + offsetof(scene_asset_light_t, entity), VIS_LAMP);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + VIS_CHILD * sizeof(uint16_t), VIS_PARENT);
    const scene_asset_timeline_t timeline = {.clip = "timeline.clip", .wrap = ANIM_CLAMP};
    memcpy(part_row(entry, SCENE_ASSET_TIMELINE), &timeline, sizeof timeline);
    uint8_t* clip = test_pack_add(pack, CLIP, ANIM_TRACKS_ASSET, CLIP_BYTES);

    enum { LIGHT_KEYS = 4, TRACK_STRIDE = 2 * LIGHT_KEYS * sizeof(float), VIS_TRACKS = VIS_ROWS - 1 };

    test_tracks_header(clip, VIS_TRACKS, DURATION);
    const uint32_t entity_component = ASSET_TYPE('E', 'N', 'T', 'Y');
    const char* names[] = {"red", "green", "blue", "lamp", "parent", "child"};
    const uint32_t starts[] = {VIS_FIRST, VIS_SECOND, VIS_THIRD, VIS_FIRST, DURATION, VIS_SECOND};
    for (int i = 0; i < VIS_TRACKS; i++) {
        const bool light = i == VIS_LAMP - 1;
        const bool child = i == VIS_CHILD - 1;
        const uint32_t times = TIMES + i * TRACK_STRIDE;
        const uint32_t values = times + LIGHT_KEYS * sizeof(float);
        const float seconds[] = {0, (float)starts[i] / MATH_MS_PER_S, (float)VIS_DARK / MATH_MS_PER_S,
                                 (float)VIS_RELIT / MATH_MS_PER_S};
        const float enabled[] = {0, child ? -1.0F : 1.0F, 0, 1.0F};
        const int keys = light ? LIGHT_KEYS : i == VIS_PARENT - 1 ? 1 : KEYS;
        test_track_row(
            clip, i,
            &(test_track_t){names[i], "enabled", entity_component, ANIM_VALUE_FLOAT, times, values, keys, interp});
        test_pack_put_floats(clip + times, seconds, keys);
        test_pack_put_floats(clip + values, enabled, keys);
    }
}

static void
assert_enabled_frame(scene_t* played, scene_t* sought, uint32_t now) {
    const uint8_t state = SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE;
    for (int e = 0; e < VIS_ROWS; e++) {
        TEST_ASSERT_EQUAL_UINT8(played->flags[e] & state, sought->flags[e] & state);
    }
    const uint32_t starts[] = {VIS_FIRST, VIS_SECOND, VIS_THIRD};
    int drawable = 0;
    for (int e = 0; e < VIS_COPIES; e++) {
        const scene_skinned_renderer_t* a = &played->skinned_renderers[e];
        const scene_skinned_renderer_t* b = &sought->skinned_renderers[e];
        if (now < starts[e]) {
            TEST_ASSERT_EQUAL_UINT8(0, sought->flags[e] & state);
            TEST_ASSERT_NULL(b->palette);
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8(state, sought->flags[e] & state);
        TEST_ASSERT_NOT_NULL(a->palette);
        TEST_ASSERT_NOT_NULL(b->palette);
        TEST_ASSERT_EQUAL_MEMORY(a->palette, b->palette, b->skin.joint_count * sizeof *b->palette);
        const scene_renderer_t* r = &sought->renderers[scene_renderer_index(sought, e)];
        TEST_ASSERT_EQUAL_PTR(b->output.positions, r->mesh.positions);
        drawable++;
    }
    scene_resolve(sought);
    TEST_ASSERT_EQUAL_INT(drawable, sought->active_renderer_count);
    TEST_ASSERT_EQUAL_UINT8(0, sought->flags[VIS_PARENT] & state);
    TEST_ASSERT_EQUAL_UINT8(now >= VIS_SECOND ? SCENE_FLAG_ENABLED : 0, sought->flags[VIS_CHILD] & state);
    const bool lit = (now >= VIS_FIRST && now < VIS_DARK) || now >= VIS_RELIT;
    TEST_ASSERT_EQUAL_UINT8(lit ? state : 0, sought->flags[VIS_LAMP] & state);
    TEST_ASSERT_EQUAL_INT(lit, sought->world_lights.count);
    TEST_ASSERT_EQUAL_INT(played->world_lights.count, sought->world_lights.count);
    TEST_ASSERT_EQUAL_MEMORY(played->world_lights.direction, sought->world_lights.direction,
                             sizeof sought->world_lights.direction);
    TEST_ASSERT_EQUAL_MEMORY(played->world_lights.color, sought->world_lights.color, sizeof sought->world_lights.color);
}

static void
test_enabled_seek_equals_play(void) {
    test_pack_t writer;
    enabled_fixture(&writer, ANIM_STEP);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack), initial = load(&pack);
    entity_set_enabled(entity_find(sought, name_hash("blue")), true);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    timeline_play(played);
    uint32_t previous = 0;
    const uint32_t edges[] = {VIS_FIRST, VIS_DARK, VIS_SECOND, VIS_RELIT, VIS_THIRD};
    for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) {
        const uint32_t now = edges[i];
        scene_animation_step(a, now - previous);
        timeline_seek(sought, now);
        assert_enabled_frame(a, b, now);
        previous = now;
    }
    timeline_seek(sought, 0);
    assert_enabled_frame(columns(initial), b, 0);
}

static void
test_enabled_refuses_nonstep_keys(void) {
    test_pack_t writer;
    enabled_fixture(&writer, ANIM_STEP);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_FALSE(scene_failure(load(&pack), &failure));
    release_fixture();
    enabled_fixture(&writer, ANIM_LINEAR);
    open_pack(&writer, &pack);
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
    TEST_ASSERT_EQUAL_STRING("red:ENTY.enabled", failure.what);
}

static __attribute__((noinline)) void
assert_weighted_pose(entity_t root, uint32_t sample, uint32_t lag, float weight) {
    const scene_t* scene = columns(root);
    timeline_seek(root, sample);
    const transformf_t own = scene_timeline_local_at(scene, CAMERA, &scene->timeline->clock, transformf_identity());
    const transformf_t* actual = &scene->locals[CAMERA];
    TEST_ASSERT_EQUAL_MEMORY(&own.scale, &actual->scale, sizeof own.scale);
    if (weight == 0.0F) {
        TEST_ASSERT_EQUAL_MEMORY(&own.position, &actual->position, sizeof own.position);
        TEST_ASSERT_EQUAL_MEMORY(&own.rotation, &actual->rotation, sizeof own.rotation);
    } else {
        const float fraction = (float)(sample - lag) / DURATION;
        const vec3f_t follow = {1.0F + 3.0F * fraction, 0, 0};
        const vec3f_t look = scene_timeline_world_at(scene, BUTTERFLY, sample).position;
        const vec3f_t position = vec3f_lerp(own.position, follow, weight);
        const quatf_t rotation = quatf_nlerp(own.rotation, rig_look_rotation(position, look, vec3f_up()), weight);
        TEST_ASSERT_EQUAL_FLOAT(position.x, actual->position.x);
        TEST_ASSERT_EQUAL_FLOAT(position.y, actual->position.y);
        TEST_ASSERT_EQUAL_FLOAT(position.z, actual->position.z);
        TEST_ASSERT_EQUAL_FLOAT(1.0F, fabsf(quatf_mul(quatf_conjugate(rotation), actual->rotation).w));
    }
}

static void
test_constraint_follows_lagged_transform(void) {
    enum { LAG_MS = 200, SAMPLE_MS = 700, SCALE_TRACK = TRACKS - 1 };

    const float weights[] = {0.0F, 1.0F, 0.35F};
    for (unsigned i = 0; i < sizeof weights / sizeof *weights; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        const scene_asset_constraint_t constraint = {.entity = CAMERA,
                                                     .follow = name_hash(CAPYBARA_NAME),
                                                     .look_at = name_hash(BUTTERFLY_NAME),
                                                     .lag_ms = LAG_MS,
                                                     .weight = weights[i]};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &constraint, sizeof constraint);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const test_track_t scale = {
            CAMERA_NAME, "scale", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SCALE_VALUES, KEYS, ANIM_LINEAR};
        test_track_row(clip, SCALE_TRACK, &scale);
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), SUBJECT);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        assert_weighted_pose(load(&pack), SAMPLE_MS, LAG_MS, weights[i]);
        release_fixture();
    }
}

static void
constraint_target_keys(uint8_t* clip, size_t at, const name_hash_t targets[BLOCK_KEYS]) {
    for (int key = 0; key < BLOCK_KEYS; key++) {
        float parts[ANIM_NAME_WIDTH];
        anim_name_encode(targets[key], parts);
        test_pack_put_floats(clip + at + key * sizeof parts, parts, ANIM_NAME_WIDTH);
    }
}

static void
constraint_tracks(test_pack_t* writer, bool animate_constraint) {
    uint8_t* clip = writer->bytes + writer->end - test_pack_aligned(CLIP_BYTES);
    const int extra = animate_constraint ? CNST_TRACKS : CNST_TRANSFORM_TRACKS;
    const size_t initial = TEST_TRACKS_HEADER_SIZE + TRACKS * TEST_TRACK_ROW_SIZE;
    memmove(clip + initial + extra * TEST_TRACK_ROW_SIZE, clip + initial, TRACKS * TEST_TRACK_STRING_SLOT);
    test_tracks_header(clip, TRACKS + extra, DURATION);
    const test_track_t tracks[] = {
        {BUTTERFLY_NAME, POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, CNST_POSITION, KEYS, ANIM_LINEAR},
        {BUTTERFLY_NAME, "rotation", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, CNST_ROTATION, KEYS,
         ANIM_LINEAR},
        {CAMERA_NAME, "follow", ANIM_COMPONENT_CONSTRAINT, ANIM_VALUE_NAME, CNST_TIMES, CNST_FOLLOW, BLOCK_KEYS,
         ANIM_STEP},
        {CAMERA_NAME, "look_at", ANIM_COMPONENT_CONSTRAINT, ANIM_VALUE_NAME, CNST_TIMES, CNST_LOOK, BLOCK_KEYS,
         ANIM_STEP},
        {CAMERA_NAME, "weight", ANIM_COMPONENT_CONSTRAINT, ANIM_VALUE_FLOAT, CNST_WEIGHT_TIMES, CNST_WEIGHT, BLOCK_KEYS,
         ANIM_LINEAR},
        {CAMERA_NAME, "lag_ms", ANIM_COMPONENT_CONSTRAINT, ANIM_VALUE_FLOAT, CNST_TIMES, CNST_LAG, BLOCK_KEYS,
         ANIM_STEP},
        {CAMERA_NAME, "handoff_ms", ANIM_COMPONENT_CONSTRAINT, ANIM_VALUE_FLOAT, CNST_TIMES, CNST_HANDOFF, BLOCK_KEYS,
         ANIM_STEP},
    };
    for (int i = 0; i < extra; i++) {
        test_track_row(clip, TRACKS + i, &tracks[i]);
    }
    const float positions[] = {-4, 2, 7, 8, 3, 9};
    const quatf_t tilt = quatf_from_euler((vec3f_t){0.4F, 0.25F, 0.5F});
    const float rotations[] = {0, 0, 0, 1, tilt.x, tilt.y, tilt.z, tilt.w};
    const float times[] = {0, (float)CNST_FIRST_CHANGE / MATH_MS_PER_S, (float)CNST_LAST_CHANGE / MATH_MS_PER_S};
    const float weight_times[] = {0, 0.55F, 1.0F};
    const float weights[] = {0, 1, 0.5F};
    test_pack_put_floats(clip + CNST_POSITION, positions, sizeof positions / sizeof *positions);
    test_pack_put_floats(clip + CNST_ROTATION, rotations, sizeof rotations / sizeof *rotations);
    test_pack_put_floats(clip + CNST_TIMES, times, BLOCK_KEYS);
    test_pack_put_floats(clip + CNST_WEIGHT_TIMES, weight_times, BLOCK_KEYS);
    test_pack_put_floats(clip + CNST_WEIGHT, weights, BLOCK_KEYS);
    const float lags[] = {175, 175, 225};
    const float durations[] = {150, 150, 150};
    test_pack_put_floats(clip + CNST_LAG, lags, BLOCK_KEYS);
    test_pack_put_floats(clip + CNST_HANDOFF, durations, BLOCK_KEYS);
    const name_hash_t follow[] = {name_hash(BUTTERFLY_NAME), name_hash(CAPYBARA_NAME), NAME_HASH_NONE};
    const name_hash_t look[] = {NAME_HASH_NONE, follow[1], follow[0]};
    constraint_target_keys(clip, CNST_FOLLOW, follow);
    constraint_target_keys(clip, CNST_LOOK, look);
}

static void
test_constraint_lag_across_wrap(void) {
    enum { LAG = 200 };

    for (int wrap = ANIM_CLAMP; wrap <= ANIM_LOOP; wrap++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = wrap;
        const scene_asset_constraint_t row = {
            .entity = CAMERA, .follow = name_hash(CAPYBARA_NAME), .weight = 1.0F, .lag_ms = LAG};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        scene_t* scene = columns(root);
        const uint32_t edges[] = {0, LAG - 1, LAG, LAG + 1, DURATION - 1, DURATION, DURATION + 1};
        for (unsigned i = 0; i < sizeof edges / sizeof *edges; i++) {
            const uint32_t t = edges[i];
            const int now = wrap == ANIM_LOOP ? t % DURATION : (t > DURATION ? DURATION : t);
            const int delayed = now - LAG;
            const int sample = delayed >= 0 ? delayed : wrap == ANIM_LOOP ? delayed + DURATION : 0;
            timeline_seek(root, 0);
            timeline_play(root);
            scene_animation_step(scene, t);
            const transformf_t played = scene_row_world(scene->root, CAMERA);
            timeline_pause(root);
            timeline_seek(root, t);
            const transformf_t sought = scene_row_world(scene->root, CAMERA);
            TEST_ASSERT_EQUAL_FLOAT(1.0F + 3.0F * sample / DURATION, sought.position.x);
            TEST_ASSERT_EQUAL_MEMORY(&played.position, &sought.position, sizeof played.position);
        }
        release_fixture();
    }
}

static void
test_constraint_reacts_without_timeline(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_constraint_t row = {.entity = CAMERA, .follow = name_hash(BUTTERFLY_NAME), .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const vec3f_t destination = {7, 8, 9};
    const entity_t target = entity_find(root, name_hash(BUTTERFLY_NAME));
    transformf_t local = transform_local(target);
    local.position = destination;
    transform_set_local(target, local);
    scene_animation_step(scene, 0);
    const transformf_t owner = scene_row_world(scene->root, CAMERA);
    TEST_ASSERT_EQUAL_MEMORY(&destination, &owner.position, sizeof destination);
}

static void
test_paused_constraint_leaves_owner_clean(void) {
    for (int timed = 0; timed <= 1; timed++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, timed, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        const scene_asset_constraint_t row = {.entity = CAMERA, .follow = name_hash(BUTTERFLY_NAME), .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), SUBJECT);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        scene_t* scene = columns(root);
        const float initial = timed ? 1.0F : 0.0F;
        TEST_ASSERT_EQUAL_FLOAT(initial, scene->locals[CAMERA].position.x);
        for (int frame = 0; frame < 3; frame++) {
            scene->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_PLACED;
            scene_animation_step(scene, STEP_MAX);
            TEST_ASSERT_EQUAL_UINT8(0, scene->flags[CAMERA] & SCENE_FLAG_PLACED);
        }
        transformf_t parent = scene->locals[SUBJECT];
        parent.scale = (vec3f_t){2.0F, 2.0F, 2.0F};
        transformf_t child = scene->locals[BUTTERFLY];
        child.position = vec3f_unit_x();
        scene_row_set_local(scene->root, BUTTERFLY, child);
        scene_row_set_local(scene->root, SUBJECT, parent);
        scene_animation_step(scene, STEP_MAX);
        const float step = timed ? 1.0F : parent.scale.x;
        TEST_ASSERT_EQUAL_FLOAT(initial + step, scene->locals[CAMERA].position.x);
        scene->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_PLACED;
        scene_animation_step(scene, STEP_MAX);
        TEST_ASSERT_EQUAL_UINT8(0, scene->flags[CAMERA] & SCENE_FLAG_PLACED);
        release_fixture();
    }
}

static void
test_constraint_uses_weighted_world_pose(void) {
    enum { FOLLOW = 1, LOOK = 2 };

    for (int mode = FOLLOW; mode <= (FOLLOW | LOOK); mode++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), SUBJECT);
        const scene_asset_constraint_t row = {
            .entity = BUTTERFLY,
            .follow = mode & FOLLOW ? name_hash(CAMERA_NAME) : NAME_HASH_NONE,
            .look_at = mode & LOOK ? name_hash(CAMERA_NAME) : NAME_HASH_NONE,
            .look_offset = {1, 2, 3},
            .weight = 0.35F,
        };
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_t* scene = columns(load(&pack));
        transformf_t* frame = &scene->locals[SUBJECT];
        frame->position = (vec3f_t){5, 7, 11};
        frame->rotation = quatf_from_axis_angle(vec3f_unit_z(), MATH_PI / 2.0F);
        frame->scale = (vec3f_t){2, 3, 4};
        const transformf_t* local = &scene->constraints[0].own;
        const transformf_t own = transformf_compose(frame, local);
        const transformf_t* target = &scene->locals[CAMERA];
        const vec3f_t eye = mode & FOLLOW ? vec3f_lerp(own.position, target->position, row.weight) : own.position;
        const vec3f_t aim = vec3f_add(target->position, row.look_offset);
        const quatf_t rotation =
            mode & LOOK ? quatf_nlerp(own.rotation, rig_look_rotation(eye, aim, vec3f_up()), row.weight) : own.rotation;
        scene_constraint_step(scene, true);
        const transformf_t got = scene_row_world(scene->root, BUTTERFLY);
        const transformf_t* result = &scene->locals[BUTTERFLY];
        const vec3f_t unrotated = quatf_rotate(quatf_conjugate(frame->rotation), vec3f_sub(eye, frame->position));
        const float tolerance = TRANSFORMF_MATRIX_TOLERANCE;
        TEST_ASSERT_FLOAT_WITHIN(tolerance, eye.x, got.position.x);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, eye.y, got.position.y);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, eye.z, got.position.z);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, unrotated.x / frame->scale.x, result->position.x);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, unrotated.y / frame->scale.y, result->position.y);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, unrotated.z / frame->scale.z, result->position.z);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 1.0F, quatf_mul(quatf_conjugate(rotation), got.rotation).w);
        const quatf_t local_rotation = quatf_mul(quatf_conjugate(frame->rotation), rotation);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 1.0F, quatf_mul(quatf_conjugate(local_rotation), result->rotation).w);
        release_fixture();
    }
}

static void
test_constraint_look_offset_uses_each_ancestor(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), SUBJECT);
    const scene_asset_constraint_t row = {
        .entity = CAMERA, .look_at = name_hash(BUTTERFLY_NAME), .look_offset = {1, 0, 0}, .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_t* scene = columns(load(&pack));
    scene->locals[SUBJECT].scale = (vec3f_t){2, 1, 1};
    scene->locals[BUTTERFLY].rotation = quatf_from_axis_angle(vec3f_unit_z(), MATH_PI / 2.0F);
    const mat4f_t world = scene_row_world_matrix(scene->root, BUTTERFLY);
    const vec3f_t aim = mat4f_apply(&world, row.look_offset);
    const vec3f_t eye = scene->constraints[0].own.position;
    const quatf_t expected = rig_look_rotation(eye, aim, vec3f_up());
    scene_constraint_step(scene, true);
    TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, 1.0F,
                             fabsf(quatf_mul(quatf_conjugate(expected), scene->locals[CAMERA].rotation).w));
}

static void
test_constraint_owner_under_zero_scale_keeps_local(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), SUBJECT);
    const scene_asset_constraint_t row = {.entity = BUTTERFLY, .follow = name_hash(CAMERA_NAME), .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const float scale[] = {1, 1, 1, 0, 0, 0};
    test_pack_put_floats(clip + SCALE_VALUES, scale, sizeof scale / sizeof *scale);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    const vec3f_t authored = scene->constraints[0].own.position;
    timeline_seek(root, DURATION);
    TEST_ASSERT_EQUAL_MEMORY(&authored, &scene->locals[BUTTERFLY].position, sizeof authored);
}

static void
assert_camera_history(asset_pack_t* pack, bool kept) {
    const entity_t root = load(pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(scene, CAMERA)));
    render_context_t* render = render_context_main();
    render->kept = true;
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_EQUAL_INT(kept, render->kept);
    render->kept = true;
    timeline_play(root);
    scene_animation_step(scene, DURATION);
    TEST_ASSERT_EQUAL_INT(kept, render->kept);
}

static void
test_constrained_camera_cuts_history(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAPYBARA_NAME, POSITION, DURATION);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    const scene_asset_constraint_t row = {.entity = CAMERA, .follow = name_hash(CAPYBARA_NAME), .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t follow = {
        CAPYBARA_NAME, POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SUBJECT_VALUES, KEYS, ANIM_LINEAR};
    test_track_row(clip, 0, &follow);
    test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    assert_camera_history(&pack, false);
}

enum { START = 100, INTERRUPT = 300, REPEAT = 350, HANDOFF = 400, RESET = 700 };

static __attribute__((noinline)) void
assert_handoff_sample(const scene_t* scene, uint32_t t, bool look, uint32_t first_duration, uint32_t duration) {
    const transformf_t own = scene_timeline_local_at(scene, CAMERA, &scene->timeline->clock, transformf_identity());
    const vec3f_t a = scene_timeline_world_at(scene, BUTTERFLY, t).position;
    const vec3f_t b = scene_timeline_world_at(scene, SUBJECT, t).position;
    const transformf_t* actual = &scene->locals[CAMERA];
    const float out = t < START ? 0 : ease_smooth(t - START, first_duration);
    const float in = t < INTERRUPT ? 0 : ease_smooth(t - INTERRUPT, duration);
    if (out == 1.0F && in == 0.0F) {
        TEST_ASSERT_EQUAL_MEMORY(&own.rotation, &actual->rotation, sizeof own.rotation);
        TEST_ASSERT_EQUAL_MEMORY(&own.position, &actual->position, sizeof own.position);
    } else if (look) {
        const quatf_t from = rig_look_rotation(own.position, a, vec3f_unit_y());
        const quatf_t to = rig_look_rotation(own.position, b, vec3f_unit_y());
        const quatf_t first = quatf_nlerp(from, own.rotation, out);
        const quatf_t expected = quatf_nlerp(first, to, in);
        const float sign = quatf_mul(quatf_conjugate(expected), actual->rotation).w < 0.0F ? -1.0F : 1.0F;
        const float tolerance = TRANSFORMF_MATRIX_TOLERANCE;
        TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.x, sign * actual->rotation.x);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.y, sign * actual->rotation.y);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.z, sign * actual->rotation.z);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.w, sign * actual->rotation.w);
        TEST_ASSERT_EQUAL_MEMORY(&own.position, &actual->position, sizeof own.position);
    } else {
        const vec3f_t expected = vec3f_lerp(vec3f_lerp(a, own.position, out), b, in);
        TEST_ASSERT_EQUAL_FLOAT(expected.x, actual->position.x);
        TEST_ASSERT_EQUAL_FLOAT(expected.y, actual->position.y);
        TEST_ASSERT_EQUAL_FLOAT(expected.z, actual->position.z);
        TEST_ASSERT_EQUAL_MEMORY(&own.rotation, &actual->rotation, sizeof own.rotation);
    }
}

static void
assert_interrupted_handoff(entity_t root, bool look, uint32_t first_duration, uint32_t duration) {
    scene_t* scene = columns(root);
    const uint32_t samples[] = {0, START + 150, INTERRUPT, REPEAT, RESET};
    uint32_t played = 0;
    timeline_play(root);
    for (unsigned i = 0; i < sizeof samples / sizeof *samples; i++) {
        const uint32_t t = samples[i];
        for (; played < t; played++) {
            scene_animation_step(scene, 1);
        }
        assert_handoff_sample(scene, t, look, first_duration, duration);
    }
}

static void
test_constraint_interrupted_handoff(void) {
    for (int mode = 0; mode < 4; mode++) {
        const bool look = (mode & 1) != 0;
        const uint32_t first_duration = mode < 2 ? HANDOFF : START;
        const uint32_t duration = mode < 2 ? HANDOFF : 0;
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        const scene_asset_constraint_t row = {.entity = CAMERA,
                                              .follow = look ? NAME_HASH_NONE : name_hash(BUTTERFLY_NAME),
                                              .look_at = look ? name_hash(BUTTERFLY_NAME) : NAME_HASH_NONE,
                                              .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
        constraint_tracks(&writer, true);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const float times[] = {(float)START / MATH_MS_PER_S, (float)INTERRUPT / MATH_MS_PER_S,
                               (float)REPEAT / MATH_MS_PER_S};
        const float durations[] = {(float)first_duration, (float)duration, 0};
        const float weights[] = {1, 1, 1};
        const float lags[BLOCK_KEYS] = {0};
        test_pack_put_floats(clip + CNST_TIMES, times, BLOCK_KEYS);
        test_pack_put_floats(clip + CNST_HANDOFF, durations, BLOCK_KEYS);
        test_pack_put_floats(clip + CNST_WEIGHT, weights, BLOCK_KEYS);
        test_pack_put_floats(clip + CNST_LAG, lags, BLOCK_KEYS);
        const name_hash_t target = name_hash(CAPYBARA_NAME);
        const name_hash_t targets[] = {NAME_HASH_NONE, target, target};
        const name_hash_t unset[BLOCK_KEYS] = {NAME_HASH_NONE, NAME_HASH_NONE, NAME_HASH_NONE};
        constraint_target_keys(clip, look ? CNST_LOOK : CNST_FOLLOW, targets);
        constraint_target_keys(clip, look ? CNST_FOLLOW : CNST_LOOK, unset);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        assert_interrupted_handoff(root, look, first_duration, duration);
        release_fixture();
    }
}

static void
test_constraint_refuses_bad_control_keys(void) {
    const struct {
        size_t values;
        float bad;
        const char* failure;
    } cases[] = {
        {CNST_LAG, -1, "camera:CNST.lag_ms"},
        {CNST_LAG, NAN, "camera:CNST.lag_ms"},
        {CNST_HANDOFF, -1, "camera:CNST.handoff_ms"},
        {CNST_HANDOFF, NAN, "camera:CNST.handoff_ms"},
        {CNST_WEIGHT, -1, "camera:CNST.weight"},
        {CNST_WEIGHT, NAN, "camera:CNST.weight"},
        {CNST_WEIGHT, 1.5F, "camera:CNST.weight"},
        {CNST_LAG, INFINITY, "camera:CNST.lag_ms"},
        {CNST_HANDOFF, INFINITY, "camera:CNST.handoff_ms"},
        {CNST_LAG, (float)ANIM_TRACKS_DURATION_MAX, "camera:CNST.lag_ms"},
        {CNST_HANDOFF, (float)ANIM_TRACKS_DURATION_MAX, "camera:CNST.handoff_ms"},
        {CNST_TIMES + sizeof(float), (float)CNST_FIRST_CHANGE / MATH_MS_PER_S, "camera:CNST.follow"},
        {CNST_TIMES + sizeof(float), (float)CNST_FIRST_CHANGE / (2 * MATH_MS_PER_S), "camera:CNST.follow"},
    };

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        constraint_tracks(&writer, true);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        test_pack_put_floats(clip + cases[i].values + sizeof(float), &cases[i].bad, 1);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
        TEST_ASSERT_EQUAL_STRING(cases[i].failure, failure.what);
        release_fixture();
    }
}

static void
test_constraint_duration_key_does_not_revive_handoff(void) {
    enum { HANDOFF = 100, LONG_HANDOFF = 800, SAMPLE = 575 };

    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_constraint_t row = {.entity = CAMERA, .follow = name_hash(BUTTERFLY_NAME), .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    constraint_tracks(&writer, true);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t handoff = {CAMERA_NAME,      "handoff_ms",      ANIM_COMPONENT_CONSTRAINT,
                                  ANIM_VALUE_FLOAT, CNST_WEIGHT_TIMES, CNST_HANDOFF,
                                  BLOCK_KEYS,       ANIM_STEP};
    test_track_row(clip, TRACKS + CNST_TRACKS - 1, &handoff);
    const float durations[] = {HANDOFF, LONG_HANDOFF, 0};
    const float weights[] = {1, 1, 1};
    const float lags[BLOCK_KEYS] = {0};
    test_pack_put_floats(clip + CNST_HANDOFF, durations, BLOCK_KEYS);
    test_pack_put_floats(clip + CNST_WEIGHT, weights, BLOCK_KEYS);
    test_pack_put_floats(clip + CNST_LAG, lags, BLOCK_KEYS);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    timeline_seek(root, SAMPLE);
    const scene_t* scene = columns(root);
    const transformf_t actual = scene_row_world(scene->root, CAMERA);
    const transformf_t expected = scene_timeline_world_at(scene, SUBJECT, SAMPLE);
    TEST_ASSERT_EQUAL_FLOAT(expected.position.x, actual.position.x);
}

static void
test_constraint_reports_unknown_row_target(void) {
    for (int track = 0; track <= 1; track++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, track, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        const name_hash_t missing = name_hash("missing");
        const scene_asset_constraint_t constraint = {
            .entity = CAMERA, .look_at = track ? NAME_HASH_NONE : missing, .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &constraint, sizeof constraint);
        if (track) {
            constraint_tracks(&writer, true);
            uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
            float value[ANIM_NAME_WIDTH];
            anim_name_encode(missing, value);
            test_pack_put_floats(clip + CNST_LOOK + (BLOCK_KEYS - 1) * sizeof value, value, ANIM_NAME_WIDTH);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
        char expected[sizeof failure.what];
        (void)snprintf(expected, sizeof expected, "camera:CNST.look_at[%08" PRIx32 "] (%s)", missing,
                       track ? "track" : "row");
        TEST_ASSERT_EQUAL_STRING(expected, failure.what);
        release_fixture();
    }
}

static void
assert_constraint_full_weight(entity_t root, uint32_t t, uint32_t lag) {
    timeline_seek(root, t);
    const scene_t* scene = columns(root);
    const transformf_t result = scene_row_world(scene->root, CAMERA);
    const float now = (float)t / DURATION;
    const float earlier = (float)(t > lag ? t - lag : 0) / DURATION;
    const vec3f_t expected_position = {2.0F + 3.0F * earlier, 2.0F, 3.0F};
    const vec3f_t expected_look = {2.0F + 4.0F * now, 2.0F * (1.0F + now), 0.0F};
    const quatf_t expected_rotation = rig_look_rotation(expected_position, expected_look, vec3f_unit_y());
    TEST_ASSERT_EQUAL_FLOAT(expected_position.x, result.position.x);
    TEST_ASSERT_EQUAL_FLOAT(expected_position.y, result.position.y);
    TEST_ASSERT_EQUAL_FLOAT(expected_position.z, result.position.z);
    const quatf_t difference = quatf_mul(quatf_conjugate(expected_rotation), result.rotation);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, fabsf(difference.w));
}

static void
test_constraint_seek_equals_play_across_wrap(void) {
    enum { LAG = 175, HANDOFF_ACROSS_END = 350 };

    for (int look = 0; look <= 1; look++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
        const scene_asset_constraint_t row = {.entity = CAMERA,
                                              .follow = look ? NAME_HASH_NONE : name_hash(BUTTERFLY_NAME),
                                              .look_at = look ? name_hash(BUTTERFLY_NAME) : NAME_HASH_NONE,
                                              .lag_ms = LAG,
                                              .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
        constraint_tracks(&writer, true);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const float weights[] = {1, 1, 1};
        const float lags[] = {LAG, LAG, LAG};
        const float handoffs[] = {HANDOFF_ACROSS_END, HANDOFF_ACROSS_END, HANDOFF_ACROSS_END};
        test_pack_put_floats(clip + CNST_WEIGHT, weights, BLOCK_KEYS);
        test_pack_put_floats(clip + CNST_LAG, lags, BLOCK_KEYS);
        test_pack_put_floats(clip + CNST_HANDOFF, handoffs, BLOCK_KEYS);
        const name_hash_t target = name_hash(CAPYBARA_NAME);
        const name_hash_t targets[] = {name_hash(BUTTERFLY_NAME), name_hash(BUTTERFLY_NAME), target};
        const name_hash_t unset[BLOCK_KEYS] = {NAME_HASH_NONE, NAME_HASH_NONE, NAME_HASH_NONE};
        constraint_target_keys(clip, look ? CNST_LOOK : CNST_FOLLOW, targets);
        constraint_target_keys(clip, look ? CNST_FOLLOW : CNST_LOOK, unset);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        scene_t* scene = columns(root);
        const uint32_t edges[] = {DURATION - 1, DURATION, DURATION + 1};
        for (unsigned i = 0; i < sizeof edges / sizeof *edges; i++) {
            timeline_seek(root, CNST_LAST_CHANGE);
            timeline_play(root);
            scene_animation_step(scene, edges[i] - CNST_LAST_CHANGE);
            const transformf_t played = scene->locals[CAMERA];
            timeline_pause(root);
            timeline_seek(root, edges[i]);
            const transformf_t* sought = &scene->locals[CAMERA];
            TEST_ASSERT_EQUAL_MEMORY(&played.position, &sought->position, sizeof played.position);
            TEST_ASSERT_EQUAL_MEMORY(&played.rotation, &sought->rotation, sizeof played.rotation);
            TEST_ASSERT_EQUAL_MEMORY(&played.scale, &sought->scale, sizeof played.scale);
            if (!look && edges[i] >= DURATION) {
                const uint32_t sample = edges[i] - LAG;
                const vec3f_t expected = scene_timeline_world_at(scene, BUTTERFLY, sample).position;
                TEST_ASSERT_EQUAL_FLOAT(expected.x, sought->position.x);
            }
        }
        release_fixture();
    }
}

static void
test_constraint_seek_equals_play(void) {
    enum { LAG = 175, HANDOFF = 150, SAMPLES = 16 };

    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_constraint_t constraint = {.entity = CAMERA,
                                                 .follow = name_hash(BUTTERFLY_NAME),
                                                 .offset = {1, 2, 3},
                                                 .look_offset = {1, 2, 0},
                                                 .handoff_ms = HANDOFF};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &constraint, sizeof constraint);
    constraint_tracks(&writer, true);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_TRUE(scene->skinned_renderers[SUBJECT].bound);
    const transformf_t initial = scene_row_local(scene->root, CAMERA);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, initial.position.x);
    TEST_ASSERT_EQUAL_FLOAT(-10.0F, initial.position.z);
    const quatf_t rotation = quatf_identity();
    TEST_ASSERT_EQUAL_MEMORY(&rotation, &initial.rotation, sizeof rotation);
    assert_constraint_full_weight(root, CNST_FIRST_CHANGE + HANDOFF, LAG);
    const uint32_t edges[] = {0,
                              LAG - 1,
                              LAG,
                              LAG + 1,
                              CNST_FIRST_CHANGE - 1,
                              CNST_FIRST_CHANGE,
                              CNST_FIRST_CHANGE + 1,
                              CNST_FIRST_CHANGE + HANDOFF - 1,
                              CNST_FIRST_CHANGE + HANDOFF,
                              CNST_FIRST_CHANGE + HANDOFF + 1,
                              CNST_LAST_CHANGE - 1,
                              CNST_LAST_CHANGE,
                              CNST_LAST_CHANGE + 1,
                              CNST_LAST_CHANGE + HANDOFF - 1,
                              CNST_LAST_CHANGE + HANDOFF,
                              CNST_LAST_CHANGE + HANDOFF + 1,
                              DURATION - 1,
                              DURATION};
    for (int mode = 0; mode < 3; mode++) {
        rng_t rng;
        rng_seed(&rng, RANDOM_SEED + (uint32_t)mode);
        for (unsigned i = 0; i < SAMPLES + sizeof edges / sizeof *edges; i++) {
            const uint32_t start = i < SAMPLES / 2 ? CNST_FIRST_CHANGE : CNST_LAST_CHANGE;
            const uint32_t t = i >= SAMPLES ? edges[i - SAMPLES]
                               : i % 2      ? start + 1 + (uint32_t)rng_below(&rng, HANDOFF - 1)
                                            : (uint32_t)rng_below(&rng, DURATION);
            timeline_seek(root, 0);
            timeline_play(root);
            for (uint32_t elapsed = 0; elapsed < t;) {
                uint32_t dt = 1 + (uint32_t)rng_below(&rng, STEP_MAX);
                if (dt > t - elapsed) {
                    dt = t - elapsed;
                }
                scene_animation_step(scene, dt);
                elapsed += dt;
            }
            const transformf_t played = scene_row_local(scene->root, CAMERA);
            const transformf_t played_world = scene_row_world(scene->root, CAMERA);
            timeline_pause(root);
            timeline_seek(root, DURATION);
            timeline_seek(root, t);
            const transformf_t sought = scene_row_local(scene->root, CAMERA);
            TEST_ASSERT_EQUAL_MEMORY(&played.position, &sought.position, sizeof played.position);
            TEST_ASSERT_EQUAL_MEMORY(&played.rotation, &sought.rotation, sizeof played.rotation);
            TEST_ASSERT_EQUAL_MEMORY(&played.scale, &sought.scale, sizeof played.scale);
            const transformf_t sought_world = scene_row_world(scene->root, CAMERA);
            TEST_ASSERT_EQUAL_MEMORY(&played_world.position, &sought_world.position, sizeof played_world.position);
            TEST_ASSERT_EQUAL_MEMORY(&played_world.rotation, &sought_world.rotation, sizeof played_world.rotation);
            TEST_ASSERT_EQUAL_MEMORY(&played_world.scale, &sought_world.scale, sizeof played_world.scale);
        }
    }
}

static __attribute__((noinline)) void
assert_follow_sample(const scene_t* scene, uint32_t t, scene_row_t target_row, float weight) {
    const vec3f_t own = {3.0F + 3.0F * t / DURATION, 0.0F, -10.0F};
    const vec3f_t target = scene_timeline_world_at(scene, target_row, t).position;
    const vec3f_t expected = vec3f_lerp(own, target, weight);
    const vec3f_t actual = scene_row_world(scene->root, CAMERA).position;
    TEST_ASSERT_EQUAL_FLOAT(expected.x, actual.x);
    TEST_ASSERT_EQUAL_FLOAT(expected.y, actual.y);
    TEST_ASSERT_EQUAL_FLOAT(expected.z, actual.z);
}

static void
test_constraint_hands_off_follow_through_owner(void) {
    enum { HANDOFF = 150 };

    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_constraint_t constraint = {
        .entity = CAMERA, .follow = name_hash(BUTTERFLY_NAME), .handoff_ms = HANDOFF, .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &constraint, sizeof constraint);
    constraint_tracks(&writer, true);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const name_hash_t targets[] = {name_hash(BUTTERFLY_NAME), NAME_HASH_NONE, name_hash(CAPYBARA_NAME)};
    const float weights[] = {1.0F, 1.0F, 1.0F};
    for (int key = 0; key < BLOCK_KEYS; key++) {
        float parts[ANIM_NAME_WIDTH];
        anim_name_encode(targets[key], parts);
        test_pack_put_floats(clip + CNST_FOLLOW + key * sizeof parts, parts, ANIM_NAME_WIDTH);
    }
    test_pack_put_floats(clip + CNST_WEIGHT, weights, BLOCK_KEYS);
    const float lags[BLOCK_KEYS] = {0};
    test_pack_put_floats(clip + CNST_LAG, lags, BLOCK_KEYS);

    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_failure_t failure;
    TEST_ASSERT_FALSE(scene_failure(root, &failure));
    scene_t* scene = columns(root);

    const struct {
        uint32_t time;
        scene_row_t target;
        float weight;
    } samples[] = {
        {0, BUTTERFLY, 1.0F},
        {CNST_FIRST_CHANGE + HANDOFF / 2, BUTTERFLY, 0.5F},
        {CNST_FIRST_CHANGE + HANDOFF, BUTTERFLY, 0.0F},
        {CNST_LAST_CHANGE + HANDOFF / 2, SUBJECT, 0.5F},
        {CNST_LAST_CHANGE + HANDOFF, SUBJECT, 1.0F},
    };

    timeline_play(root);
    uint32_t played = 0;
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        const uint32_t t = samples[i].time;
        for (; played < t; played++) {
            scene_animation_step(scene, 1);
        }
        assert_follow_sample(scene, t, samples[i].target, samples[i].weight);
    }
}

static __attribute__((noinline)) float
sample_world_x(const scene_t* scene, scene_row_t row, uint32_t ms) {
    return scene_timeline_world_at(scene, row, ms).position.x;
}

static void
test_timeline_samples_target_ancestors_at_requested_time(void) {
    enum { SAMPLE = 250, CURRENT = 900, PAST_END = DURATION + SAMPLE };

    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), SUBJECT);
    const scene_asset_constraint_t row = {
        .entity = CAMERA, .follow = name_hash(BUTTERFLY_NAME), .lag_ms = CURRENT - SAMPLE, .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
    constraint_tracks(&writer, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    transformf_t placement = transformf_identity();
    placement.position = vec3f_unit_x();
    scene_row_set_local(scene->root, SCENE_ROW_ROOT, placement);
    timeline_seek(root, CURRENT);

    const float fraction = (float)SAMPLE / DURATION;
    const float target = -4.0F + 12.0F * fraction;
    const float expected = 1.0F + 1.0F + 3.0F * fraction + (1.0F + fraction) * target;
    TEST_ASSERT_EQUAL_FLOAT(expected, sample_world_x(scene, BUTTERFLY, SAMPLE));
    TEST_ASSERT_EQUAL_FLOAT(expected, scene_row_world(scene->root, CAMERA).position.x);
    const vec3f_t child = scene->locals[BUTTERFLY].position;
    const vec3f_t parent = scene->locals[SUBJECT].position;
    const uint8_t child_flags = scene->flags[BUTTERFLY];
    const uint8_t parent_flags = scene->flags[SUBJECT];
    const float end = 1.0F + 4.0F + 2.0F * 8.0F;
    TEST_ASSERT_EQUAL_FLOAT(end, sample_world_x(scene, BUTTERFLY, PAST_END));
    TEST_ASSERT_EQUAL_MEMORY(&child, &scene->locals[BUTTERFLY].position, sizeof child);
    TEST_ASSERT_EQUAL_MEMORY(&parent, &scene->locals[SUBJECT].position, sizeof parent);
    TEST_ASSERT_EQUAL_UINT8(child_flags, scene->flags[BUTTERFLY]);
    TEST_ASSERT_EQUAL_UINT8(parent_flags, scene->flags[SUBJECT]);
    TEST_ASSERT_EQUAL_UINT32(CURRENT, timeline_time_ms(root));
}

static __attribute__((noinline)) void
assert_bind_pose(entity_t root, vec3f_t offset, rig_bind_t bind) {
    const scene_t* scene = columns(root);
    const transformf_t owner = scene_row_world(scene->root, CAMERA);
    const transformf_t target = scene_row_world(scene->root, BUTTERFLY);
    const vec3f_t expected = rig_follow_position(&target, offset, bind);
    TEST_ASSERT_EQUAL_FLOAT(expected.x, owner.position.x);
    TEST_ASSERT_EQUAL_FLOAT(expected.y, owner.position.y);
    TEST_ASSERT_EQUAL_FLOAT(expected.z, owner.position.z);
    const transformf_t local = scene_row_local(scene->root, CAMERA);
    const transformf_t own = scene_timeline_local_at(scene, CAMERA, &scene->timeline->clock, transformf_identity());
    TEST_ASSERT_EQUAL_MEMORY(&own.rotation, &local.rotation, sizeof own.rotation);
}

static void
test_constraint_bind_modes_and_unset_look(void) {
    static const rig_bind_t binds[] = {RIG_BIND_WORLD, RIG_BIND_TARGET, RIG_BIND_TARGET_YAW};
    for (int i = 0; i < (int)(sizeof binds / sizeof *binds); i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        const scene_asset_constraint_t constraint = {.entity = CAMERA,
                                                     .bind = binds[i],
                                                     .follow = name_hash(BUTTERFLY_NAME),
                                                     .offset = {1, 2, 3},
                                                     .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &constraint, sizeof constraint);
        constraint_tracks(&writer, false);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        timeline_seek(root, DURATION / 2);
        assert_bind_pose(root, constraint.offset, binds[i]);
        release_fixture();
    }
}

static void
test_constraint_refuses_its_own_output_as_input(void) {
    for (int keyed = 0; keyed <= 1; keyed++) {
        for (int descendant = 0; descendant <= 1; descendant++) {
            for (int look = 0; look <= 1; look++) {
                test_pack_t writer;
                uint8_t* entry = fixture(&writer, keyed, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
                if (descendant) {
                    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + BUTTERFLY * sizeof(uint16_t), CAMERA);
                }
                const name_hash_t target = name_hash(descendant ? BUTTERFLY_NAME : CAMERA_NAME);
                const scene_asset_constraint_t row = {
                    .entity = CAMERA,
                    .follow = !keyed && !look ? target : NAME_HASH_NONE,
                    .look_at = !keyed && look ? target : NAME_HASH_NONE,
                    .weight = 1.0F,
                };
                memcpy(part_row(entry, SCENE_ASSET_CONSTRAINTS), &row, sizeof row);
                if (keyed) {
                    constraint_tracks(&writer, true);
                    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
                    float parts[ANIM_NAME_WIDTH];
                    anim_name_encode(target, parts);
                    test_pack_put_floats(clip + (look ? CNST_LOOK : CNST_FOLLOW), parts, ANIM_NAME_WIDTH);
                    anim_name_encode(name_hash(CAPYBARA_NAME), parts);
                    test_pack_put_floats(clip + (look ? CNST_FOLLOW : CNST_LOOK), parts, ANIM_NAME_WIDTH);
                }
                asset_pack_t pack;
                open_pack(&writer, &pack);
                scene_failure_t failure;
                TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
                TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
                char expected[sizeof failure.what];
                (void)snprintf(expected, sizeof expected, "%s:CNST.%s[%08" PRIx32 "]%s", CAMERA_NAME,
                               look ? "look_at" : "follow", target, CAMERA_NAME);
                TEST_ASSERT_EQUAL_STRING(expected, failure.what);
                release_fixture();
            }
        }
    }
}

static void
test_constraint_refuses_transform_writer_conflicts(void) {
    enum {
        TARGET_PROPERTY,
        OWNER_PROPERTY,
        TARGET_ANCESTOR,
        OWNER_ANCESTOR,
        TARGET_CONSTRAINT,
        DUPLICATE,
        TARGET_HINGE,
        OWNER_HINGE,
        CASES
    };

    for (int i = 0; i < CASES; i++) {
        const bool property = i <= OWNER_ANCESTOR;
        const bool hinge = i == TARGET_HINGE || i == OWNER_HINGE;
        const int constraints = i == TARGET_CONSTRAINT || i == DUPLICATE ? 2 : 1;
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, constraints, hinge, property, CAMERA_NAME, POSITION, DURATION);
        const bool targets_camera = i == TARGET_PROPERTY || i == TARGET_HINGE;
        const scene_asset_constraint_t constraint = {.entity = targets_camera || i == OWNER_ANCESTOR ? BUTTERFLY
                                                               : i == TARGET_ANCESTOR                ? SUBJECT
                                                                                                     : CAMERA,
                                                     .follow = i == TARGET_ANCESTOR ? name_hash(BUTTERFLY_NAME)
                                                               : targets_camera     ? name_hash(CAMERA_NAME)
                                                                                    : name_hash(CAPYBARA_NAME),
                                                     .weight = 1.0F};
        uint8_t* rows = part_row(entry, SCENE_ASSET_CONSTRAINTS);
        memcpy(rows, &constraint, sizeof constraint);
        if (i == TARGET_ANCESTOR || i == OWNER_ANCESTOR) {
            const scene_row_t child = BUTTERFLY;
            test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + child * sizeof(uint16_t), CAMERA);
        }
        if (constraints > 1) {
            const scene_asset_constraint_t other = {.entity = i == DUPLICATE ? CAMERA : SUBJECT, .weight = 1.0F};
            memcpy(rows + sizeof other, &other, sizeof other);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
        if (i == OWNER_HINGE || i == OWNER_ANCESTOR || i == OWNER_PROPERTY) {
            TEST_ASSERT_EQUAL_STRING(i == OWNER_HINGE      ? "camera:HNGE.angle"
                                     : i == OWNER_ANCESTOR ? CAMERA_NAME
                                                           : ANIM_TARGET_SELF ":TRNS.position",
                                     failure.what);
        } else if (i == DUPLICATE) {
            TEST_ASSERT_EQUAL_STRING(SCENE_TEST_TIMELINE_ID, failure.what);
        } else {
            const char* target = targets_camera ? CAMERA_NAME : i == TARGET_ANCESTOR ? BUTTERFLY_NAME : CAPYBARA_NAME;
            const char* driver = i == TARGET_CONSTRAINT ? CAPYBARA_NAME : CAMERA_NAME;
            char expected[sizeof failure.what];
            (void)snprintf(expected, sizeof expected, "%s:CNST.follow[%08" PRIx32 "]%s",
                           i == TARGET_CONSTRAINT ? CAMERA_NAME
                           : i == TARGET_ANCESTOR ? CAPYBARA_NAME
                                                  : BUTTERFLY_NAME,
                           name_hash(target), driver);
            TEST_ASSERT_EQUAL_STRING(expected, failure.what);
        }
        release_fixture();
    }
}

static void
test_clamp_samples_fractional_final_step_key(void) {
    enum { CLAMP_MS = 100 };

    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, CLAMP_MS);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const float times[] = {0.0F, 0.1004F};
    test_pack_put_floats(clip + TIMES, times, KEYS);
    clip[TEST_TRACKS_HEADER_SIZE + ANIM_TRACKS_ROW_INTERP] = ANIM_STEP;
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    timeline_seek(root, CLAMP_MS - 1);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, scene->locals[CAMERA].position.x);
    timeline_seek(root, CLAMP_MS);
    TEST_ASSERT_EQUAL_FLOAT(6.0F, scene->locals[CAMERA].position.x);
}

static void
test_property_clamp_samples_fractional_final_step_key(void) {
    enum { CLAMP_MS = 100 };

    test_pack_t writer;
    fixture(&writer, 0, 0, 0, true, CAMERA_NAME, POSITION, CLAMP_MS);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const float times[] = {0.0F, 0.1004F};
    test_pack_put_floats(clip + TIMES, times, KEYS);
    clip[TEST_TRACKS_HEADER_SIZE + ANIM_TRACKS_ROW_INTERP] = ANIM_STEP;
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_t* scene = columns(load(&pack));
    const entity_t camera = scene_row_entity(scene, CAMERA);
    const char* clips[] = {CLIP};
    TEST_ASSERT_TRUE(animator_set_clips(camera, NULL, clips, 1, NULL));
    scene->animators[CAMERA].wrap = ANIM_CLAMP;
    TEST_ASSERT_TRUE(animator_play(camera, 0, 0));
    TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, CLAMP_MS - 1));
    TEST_ASSERT_EQUAL_FLOAT(3.0F, scene->locals[CAMERA].position.x);
    scene_animation_step(scene, 1);
    TEST_ASSERT_EQUAL_FLOAT(6.0F, scene->locals[CAMERA].position.x);
}

static void
test_timeline_and_disabled_animator_seek_to_the_same_boundary(void) {
    const anim_wrap_t wraps[] = {ANIM_CLAMP, ANIM_LOOP};
    for (size_t mode = 0; mode < sizeof wraps / sizeof *wraps; mode++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, true, "red", POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = wraps[mode];
        part_row(entry, SCENE_ASSET_ANIMATORS)[offsetof(scene_asset_animator_t, wrap)] = wraps[mode];
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 1);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        scene_t* scene = columns(root);
        const entity_t camera = scene_row_entity(scene, CAMERA);
        entity_set_enabled(camera, false);
        const uint32_t target = DURATION + DURATION / 2;
        timeline_seek(root, target);
        TEST_ASSERT_TRUE(animator_seek(camera, ANIMATOR_CURRENT, target));
        const uint32_t expected = wraps[mode] == ANIM_LOOP ? DURATION / 2 : DURATION;
        TEST_ASSERT_EQUAL_UINT32(expected, timeline_time_ms(root));
        TEST_ASSERT_EQUAL_UINT32(expected, scene->animators[CAMERA].layer.current.playhead.t_ms);
        release_fixture();
    }
}

static void
test_animator_and_timeline_can_write_different_camera_fields(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, true, CAMERA_NAME, POSITION, DURATION);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t lens = {CAMERA_NAME, "near_z",   ANIM_COMPONENT_CAMERA, ANIM_VALUE_FLOAT, TIMES, NEAR_VALUES,
                               KEYS,        ANIM_LINEAR};
    test_track_row(clip, 0, &lens);
    test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    timeline_seek(root, DURATION);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, scene->cameras[0].lens.near_z);
    TEST_ASSERT_TRUE(animator_seek(scene_row_entity(scene, CAMERA), ANIMATOR_CURRENT, DURATION / 2));
    TEST_ASSERT_EQUAL_FLOAT(2.5F, scene->locals[CAMERA].position.x);
}

static void
test_nondriven_active_camera_keeps_history(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 0, false, "red", POSITION, DURATION);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    assert_camera_history(&pack, true);
}

static void
test_timeline_clip_type_root_and_empty_tracks(void) {
    for (int i = 0; i < 3; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        const bool wrong_type = i == 0;
        const bool wrong_root = i == 1;
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        if (wrong_type) {
            const scene_asset_timeline_t row = {.clip = "red", .wrap = ANIM_CLAMP};
            memcpy(part_row(entry, SCENE_ASSET_TIMELINE), &row, sizeof row);
        } else if (wrong_root) {
            clip[ANIM_TRACKS_AT_ROOT] = ANIM_ROOT_SKELETON;
        } else {
            test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 0);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        if (wrong_type || wrong_root) {
            scene_failure_t failure;
            TEST_ASSERT_TRUE(scene_failure(root, &failure));
            TEST_ASSERT_EQUAL_INT(wrong_type ? ASSET_ERR_TYPE : ASSET_ERR_FORMAT, failure.asset);
            TEST_ASSERT_EQUAL_STRING(wrong_type ? "red" : CLIP, failure.what);
        } else {
            TEST_ASSERT_NOT_NULL(columns(root));
            timeline_seek(root, DURATION);
            TEST_ASSERT_EQUAL_UINT32(DURATION, timeline_time_ms(root));
        }
        release_fixture();
    }
}

static void
test_refused_scene_leaves_lender_untouched(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, true, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    arena = memory_alloc(ARENA_BYTES, MEMORY_APP);
    TEST_ASSERT_NOT_NULL(arena);
    (void)scene_set_take(lend_timeline_memory);
    for (int attempt = 0; attempt < SCENE_ROOTS_MAX; attempt++) {
        const entity_t refused = load(&pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(refused, &failure));
        TEST_ASSERT_EQUAL_STRING(ANIM_TARGET_SELF ":TRNS.position", failure.what);
        TEST_ASSERT_EQUAL_size_t(0, arena_used);
        for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
            TEST_ASSERT_NULL(scene_roots[i].scene);
        }
        scene_release(refused);
    }
    const entity_t good = scene_load_from(&pack, SCENE_TEST_STATIC_ID);
    TEST_ASSERT_NOT_NULL(columns(good));
}

static void
test_pause_seek_backwards_and_preview(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const entity_t fresh = load(&pack);
    scene_t* scene = columns(root);
    timeline_play(root);
    scene_animation_step(scene, DURATION - 1);
    TEST_ASSERT_EQUAL_UINT32(DURATION / 2, preview_timeline(root));
    TEST_ASSERT_FALSE(timeline_playing(root));
    timeline_seek(fresh, DURATION / 2);
    const vec3f_t expected = columns(fresh)->locals[CAMERA].position;
    TEST_ASSERT_EQUAL_MEMORY(&expected, &scene->locals[CAMERA].position, sizeof expected);
    timeline_seek(root, DURATION / 4);
    TEST_ASSERT_EQUAL_FLOAT(3.75F, scene->locals[CAMERA].position.x);
    scene_animation_step(scene, DURATION);
    TEST_ASSERT_EQUAL_UINT32(DURATION / 4, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_FLOAT(3.75F, scene->locals[CAMERA].position.x);

    timeline_play(root);
    scene_set_paused(true);
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_EQUAL_MEMORY(&expected, &scene->locals[CAMERA].position, sizeof expected);
    scene_set_paused(false);
}

static void
test_timeline_boundaries_seek_and_step(void) {
    const uint32_t times[] = {0, DURATION - 1, DURATION, 2 * DURATION + DURATION / 2};
    for (int wrap = ANIM_CLAMP; wrap <= ANIM_LOOP; wrap++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = wrap;
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        const entity_t reference = load(&pack);
        scene_t* scene = columns(root);
        for (size_t i = 0; i < sizeof times / sizeof *times; i++) {
            timeline_seek(root, 0);
            timeline_play(root);
            scene_timeline_step(scene, times[i]);
            timeline_seek(reference, times[i]);
            const uint32_t expected =
                wrap == ANIM_LOOP ? times[i] % DURATION : (times[i] < DURATION ? times[i] : DURATION);
            TEST_ASSERT_EQUAL_UINT32(expected, timeline_time_ms(root));
            TEST_ASSERT_EQUAL_UINT32(expected, timeline_time_ms(reference));
            TEST_ASSERT_EQUAL_MEMORY(&columns(reference)->locals[CAMERA].position, &scene->locals[CAMERA].position,
                                     sizeof(vec3f_t));
        }
        release_fixture();
    }
}

static void
test_timeline_ignores_nonroot_and_released_handles(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const entity_t child = scene_row_entity(columns(root), CAMERA);
    timeline_play(child);
    timeline_seek(child, DURATION);
    timeline_pause(child);
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    TEST_ASSERT_FALSE(timeline_playing(root));
    scene_release(root);
    scene_row_t released;
    TEST_ASSERT_NULL(scene_entity_peek(root, &released));
    timeline_play(root);
    timeline_seek(root, DURATION);
    timeline_pause(root);
    TEST_ASSERT_FALSE(timeline_playing(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_duration_ms(root));
}

#if !defined(DEVICE_BUILD)
static void
test_checkpoint_restores_timeline_clock_and_locals(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    timeline_seek(root, DURATION / 4);
    const vec3f_t expected = scene->locals[CAMERA].position;
    checkpoint_t saved = {0};
    scene_checkpoint_capture(&saved, scene);
    timeline_play(root);
    timeline_seek(root, DURATION / 2);
    const bool restored = checkpoint_restore(&saved);
    checkpoint_clear(&saved);
    TEST_ASSERT_TRUE(restored);
    TEST_ASSERT_EQUAL_UINT32(DURATION / 4, timeline_time_ms(root));
    TEST_ASSERT_FALSE(timeline_playing(root));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &scene->locals[CAMERA].position, sizeof expected);
}

static void
test_keys_glb_blocks_play_through_scene_load(void) {
    enum { SWITCH_MS = 1003, BLEND_MS = 250 };

    suite_set_test_cleanup(release_fixture);
    test_asset_dir_use(".");
    const entity_t root = scene_load("keys_blocks");
    TEST_ASSERT_EQUAL_INT(PENDING_READY, scene_state(root));
    scene_t* scene = columns(root);
    const entity_t first = entity_find(root, name_hash("first"));
    scene_row_t row;
    TEST_ASSERT_EQUAL_PTR(scene->root, scene_entity_lookup(first, &row));
    const anim_layer_t* layer = &scene->animators[row].layer;
    timeline_play(root);
    scene_animation_step(scene, SWITCH_MS - 1);
    TEST_ASSERT_EQUAL_UINT8(0, layer->current.clip);
    scene_animation_step(scene, 1);
    TEST_ASSERT_EQUAL_UINT8(1, layer->current.clip);
    TEST_ASSERT_EQUAL_UINT8(0, layer->previous.clip);
    TEST_ASSERT_EQUAL_UINT32(BLEND_MS, layer->blend_ms);
    scene_animation_step(scene, BLEND_MS);
    TEST_ASSERT_EQUAL_UINT8(ANIM_LAYER_CLIP_NONE, layer->previous.clip);
}

static void
test_python_baked_v7_motion_row_layout(void) {
    suite_set_test_cleanup(release_fixture);
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(getenv("AUTANA_SCENE_PROBE"), &pack, &probe_allocation));
    asset_view_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&pack, "motion", SCENE_ASSET, &entry));
    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(entry, &asset));
    TEST_ASSERT_EQUAL_UINT16(1, asset.timeline_count);
    TEST_ASSERT_EQUAL_STRING("probe", asset.timeline->clip);
    TEST_ASSERT_EQUAL_UINT8(ANIM_LOOP, asset.timeline->wrap);
    TEST_ASSERT_EQUAL_UINT16(1, asset.constraint_count);
    const scene_asset_constraint_t* c = asset.constraints;
    TEST_ASSERT_EQUAL_UINT16(CAMERA, c->entity);
    TEST_ASSERT_EQUAL_UINT8(SCENE_ASSET_BIND_TARGET_YAW, c->bind);
    TEST_ASSERT_EQUAL_UINT32(name_hash("red"), c->follow);
    TEST_ASSERT_EQUAL_UINT32(name_hash("green"), c->look_at);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, c->offset.z);
    TEST_ASSERT_EQUAL_FLOAT(6.0F, c->look_offset.z);
    TEST_ASSERT_EQUAL_FLOAT(7.0F, c->lag_ms);
    TEST_ASSERT_EQUAL_FLOAT(8.0F, c->handoff_ms);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, c->weight);
    TEST_ASSERT_EQUAL_UINT16(1, asset.hinge_count);
    const scene_asset_hinge_t* h = asset.hinges;
    TEST_ASSERT_EQUAL_UINT16(CAMERA, h->entity);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, h->axis.y);
    TEST_ASSERT_EQUAL_FLOAT(-0.75F, h->min);
    TEST_ASSERT_EQUAL_FLOAT(0.75F, h->max);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, h->angle);
}
#endif

static void
run_scene_timeline_suite(void) {
    scene_unload_all();
    const memory_take_fn lender = scene_set_take(NULL);
    RUN_TEST(test_animator_and_timeline_can_write_different_camera_fields);
    RUN_TEST(test_nondriven_active_camera_keeps_history);
    RUN_TEST(test_timeline_clip_type_root_and_empty_tracks);
    RUN_TEST(test_refused_scene_leaves_lender_untouched);
    RUN_TEST(test_pause_seek_backwards_and_preview);
    RUN_TEST(test_timeline_boundaries_seek_and_step);
    RUN_TEST(test_timeline_ignores_nonroot_and_released_handles);
#if !defined(DEVICE_BUILD)
    RUN_TEST(test_checkpoint_restores_timeline_clock_and_locals);
    RUN_TEST(test_python_baked_v7_motion_row_layout);
    RUN_TEST(test_keys_glb_blocks_play_through_scene_load);
#endif
    RUN_TEST(test_timeline_refuses_duplicate_tracks);
    RUN_TEST(test_scene_writers_refuse_bad_lens_keys);
    RUN_TEST(test_hinge_holds_default_angle);
    RUN_TEST(test_hinge_seek_equals_play);
    RUN_TEST(test_hinge_refuses_transform_writers);
    RUN_TEST(test_enabled_seek_equals_play);
    RUN_TEST(test_enabled_refuses_nonstep_keys);
    RUN_TEST(test_constraint_follows_lagged_transform);
    RUN_TEST(test_constraint_lag_across_wrap);
    RUN_TEST(test_constraint_reacts_without_timeline);
    RUN_TEST(test_paused_constraint_leaves_owner_clean);
    RUN_TEST(test_constraint_uses_weighted_world_pose);
    RUN_TEST(test_constraint_look_offset_uses_each_ancestor);
    RUN_TEST(test_constraint_owner_under_zero_scale_keeps_local);
    RUN_TEST(test_constrained_camera_cuts_history);
    RUN_TEST(test_constraint_interrupted_handoff);
    RUN_TEST(test_constraint_duration_key_does_not_revive_handoff);
    RUN_TEST(test_constraint_refuses_bad_control_keys);
    RUN_TEST(test_constraint_reports_unknown_row_target);
    RUN_TEST(test_constraint_seek_equals_play);
    RUN_TEST(test_constraint_seek_equals_play_across_wrap);
    RUN_TEST(test_constraint_hands_off_follow_through_owner);
    RUN_TEST(test_timeline_samples_target_ancestors_at_requested_time);
    RUN_TEST(test_constraint_bind_modes_and_unset_look);
    RUN_TEST(test_constraint_refuses_its_own_output_as_input);
    RUN_TEST(test_constraint_refuses_transform_writer_conflicts);
    RUN_TEST(test_clamp_samples_fractional_final_step_key);
    RUN_TEST(test_property_clamp_samples_fractional_final_step_key);
    RUN_TEST(test_timeline_and_disabled_animator_seek_to_the_same_boundary);
    RUN_TEST(test_timeline_leaves_resting_transforms_clean);
    RUN_TEST(test_runtime_animator_cannot_take_timeline_field);
    RUN_TEST(test_camera_cut_on_loop_wrap);
    RUN_TEST(test_timeline_seek_clears_fraction);
    RUN_TEST(test_scene_rejects_overlapping_tables);
    RUN_TEST(test_animator_track_load_errors_name_the_field);
    RUN_TEST(test_rejected_non_animator_row_names_the_binding);
    RUN_TEST(test_timeline_validates_rounded_block_times);
    RUN_TEST(test_seek_matches_play_at_the_same_millisecond);
    RUN_TEST(test_clock_and_immediate_seek);
    RUN_TEST(test_zero_duration_loop_holds_zero);
    RUN_TEST(test_scene_without_a_timeline_ignores_controls);
    RUN_TEST(test_load_names_the_failing_clip_field_or_table);
    RUN_TEST(test_scne_validates_every_new_table);
    scene_unload_all();
    (void)scene_set_take(lender);
}

SUITE_REGISTER(run_scene_timeline_suite);
