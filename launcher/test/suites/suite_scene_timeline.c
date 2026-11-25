#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_file.h"
#include "math/scalar/rng.h"
#include "math/scalar/units.h"

#include "anim/anim_tracks_format.h"
#include "kinematics/rig/rig.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/light/light.h"
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
    SAMPLE_FRAME_MS = MATH_MS_PER_S / 60,
    FIRST_START = 100,
    SECOND_START = 250,
    MID_SPEED_KEY = 500,
    THIRD_START = 625,
    SECOND_BLEND = 500,
    THIRD_BLEND = 250,
    CAMERA = 1,
    SUBJECT = 0,
    ANCHOR = 2,
    MARKER = 3,
    KEYS = 2,
    TRACKS = 6,
    BLOCK_KEYS = 3,
    SPEED_KEYS = 4,
    BLOCK_TRACKS = TRACKS + BLOCK_KEYS,
    CONSTRAINT_TRACKS = 3,
    TRACK_CAPACITY = BLOCK_TRACKS + CONSTRAINT_TRACKS,
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
    WEIGHT_VALUES = SPEED_VALUES + SPEED_KEYS * sizeof(float),
    CLIP_BYTES = WEIGHT_VALUES + CONSTRAINT_TRACKS * KEYS * sizeof(float)
};

static const char CLIP[] = "timeline.clip";
static const char CAMERA_NAME[] = "camera";
static const char LAMP_NAME[] = "lamp";
static const char POSITION[] = "position";
static const char ANCHOR_NAME[] = "anchor";

static void* allocation;
static void* skin_pressure;
static void* skin_internal_guard;
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
    test_app_heap_release(skin_pressure);
    skin_pressure = NULL;
    memory_free(skin_internal_guard);
    skin_internal_guard = NULL;
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
fixture_with_sources(test_pack_t* pack, int timelines, int positions, int aims, int hinges, bool property,
                     const char* path, const char* field, uint32_t duration, int source_count) {
    uint8_t* bytes = test_alloc_aligned(CAPACITY, ASSET_PACK_BASE_ALIGN, &allocation);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(bytes);
    const uint16_t counts[SCENE_ASSET_PART_COUNT] = {[SCENE_ASSET_TIMELINE] = timelines,
                                                     [SCENE_ASSET_POSITION_CONSTRAINTS] = positions,
                                                     [SCENE_ASSET_AIM_CONSTRAINTS] = aims,
                                                     [SCENE_ASSET_CONSTRAINT_SOURCES] = source_count,
                                                     [SCENE_ASSET_HINGES] = hinges};
    const size_t sizes[SCENE_ASSET_PART_COUNT] = {
        [SCENE_ASSET_TIMELINE] = sizeof(scene_asset_timeline_t),
        [SCENE_ASSET_POSITION_CONSTRAINTS] = sizeof(scene_asset_position_constraint_t),
        [SCENE_ASSET_AIM_CONSTRAINTS] = sizeof(scene_asset_aim_constraint_t),
        [SCENE_ASSET_CONSTRAINT_SOURCES] = sizeof(scene_asset_constraint_source_t),
        [SCENE_ASSET_HINGES] = sizeof(scene_asset_hinge_t)};
    uint8_t* entry = scene_test_timeline_entry(pack, bytes, CAPACITY, counts, sizes, property);
    uint8_t* clip = test_pack_add(pack, CLIP, ANIM_TRACKS_ASSET, CLIP_BYTES);
    test_tracks_header(clip, TRACKS, duration);
    const test_track_t tracks[] = {
        {path, field, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, CAMERA_VALUES, KEYS, ANIM_LINEAR},
        {"red", POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SUBJECT_VALUES, KEYS, ANIM_LINEAR},
        {CAMERA_NAME, "half_fov_short_tan", ANIM_COMPONENT_CAMERA, ANIM_VALUE_FLOAT, TIMES, FOV_VALUES, KEYS,
         ANIM_LINEAR},
        {CAMERA_NAME, "near_z", ANIM_COMPONENT_CAMERA, ANIM_VALUE_FLOAT, TIMES, NEAR_VALUES, KEYS, ANIM_LINEAR},
        {CAMERA_NAME, "rotation", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, ROTATION_VALUES, KEYS, ANIM_LINEAR},
        {"red", "scale", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SCALE_VALUES, KEYS, ANIM_LINEAR},
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
    if (positions || aims) {
        const scene_asset_constraint_source_t sources[] = {{name_hash(CAMERA_NAME), 1.0F},
                                                           {name_hash(ANCHOR_NAME), 0.0F}};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
    }
    if (positions) {
        const scene_asset_position_constraint_t row = {.entity = CAMERA, .source_count = 1, .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
    }
    if (aims) {
        const scene_asset_aim_constraint_t row = {.entity = CAMERA,
                                                  .source_count = 1,
                                                  .world_up_object = SCENE_ASSET_WORLD_UP_OBJECT_NONE,
                                                  .weight = 1.0F,
                                                  .aim_vector = {0, 0, 1},
                                                  .up_vector = {0, 1, 0},
                                                  .world_up_vector = {0, 1, 0},
                                                  .rotation_offset = {.w = 1.0F}};
        memcpy(part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS), &row, sizeof row);
    }
    if (hinges) {
        const scene_asset_hinge_t row = {.entity = CAMERA, .axis = {0, 0, 1}, .min = -1.0F, .max = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_HINGES), &row, sizeof row);
    }
    return entry;
}

static uint8_t*
fixture(test_pack_t* pack, int timelines, int positions, int aims, int hinges, bool property, const char* path,
        const char* field, uint32_t duration) {
    return fixture_with_sources(pack, timelines, positions, aims, hinges, property, path, field, duration,
                                positions || aims ? 2 : 0);
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
    const size_t strings = TEST_TRACKS_HEADER_SIZE + BLOCK_TRACKS * TEST_TRACK_ROW_SIZE;
    const size_t old_strings = TEST_TRACKS_HEADER_SIZE + TRACKS * TEST_TRACK_ROW_SIZE;
    memmove(clip + strings, clip + old_strings, TRACKS * TEST_TRACK_STRING_SLOT);
    test_tracks_header(clip, BLOCK_TRACKS, DURATION);
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
test_speed_key_playhead_matches_seek(void) {
    enum { SPEED_KEY_COUNT = 2 };

    const uint32_t times[] = {FIRST_START, SECOND_START, 0}, starts[] = {0};
    const float speeds[] = {0.3F, 1.25F, 0.0F};
    const uint8_t slots[] = {0};
    const anim_blocks_t blocks = {
        .starts_ms = starts, .slots = slots, .count = 1, .speed = {times, speeds, SPEED_KEY_COUNT}};
    const anim_clip_t clip = {.duration_ms = DURATION};
    anim_playhead_t played;
    anim_playhead_init(&played, ANIM_CLAMP);
    played.paused = false;
    uint32_t previous = 0;
    for (uint32_t now = FIRST_START - 1; now <= SECOND_START + 1; now++) {
        (void)anim_playhead_advance(&played, &clip, now - previous);
        const anim_block_sample_t sought = anim_blocks_sample(&blocks, &clip, ANIM_CLAMP, now);
        TEST_ASSERT_EQUAL_UINT32(played.t_ms, sought.current.playhead.t_ms);
        TEST_ASSERT_EQUAL_UINT16(played.fraction, sought.current.playhead.fraction);
        const float rate = now < FIRST_START ? 1.0F : speeds[now < SECOND_START ? 0 : 1];
        TEST_ASSERT_EQUAL_FLOAT(rate, anim_block_curve_step(&blocks.speed, now, 1.0F));
        TEST_ASSERT_TRUE(anim_playhead_set_speed(&played, rate));
        previous = now;
    }
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
        uint8_t* entry = fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
                const size_t old_strings = TEST_TRACKS_HEADER_SIZE + BLOCK_TRACKS * TEST_TRACK_ROW_SIZE;
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
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
        fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
timeline_keep_first_track(test_pack_t* writer) {
    uint8_t* clip = writer->bytes + writer->end - test_pack_aligned(CLIP_BYTES);
    test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 1);
}

static void
set_first_constraint_source(uint8_t* entry, const char* name) {
    const scene_asset_constraint_source_t source = {name_hash(name), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
}

static void
put_constraint_weights(test_pack_t* writer, int count, const char* owner, uint32_t component) {
    uint8_t* clip = writer->bytes + writer->end - test_pack_aligned(CLIP_BYTES);
    const size_t rows = TEST_TRACKS_HEADER_SIZE + count * TEST_TRACK_ROW_SIZE;
    const size_t added = CONSTRAINT_TRACKS * TEST_TRACK_ROW_SIZE;
    memmove(clip + rows + added, clip + rows, count * TEST_TRACK_STRING_SLOT);
    test_tracks_header(clip, count + CONSTRAINT_TRACKS, DURATION);
    const char* const fields[] = {"weight0", "weight1", "weight"};
    const float weights[][KEYS] = {{1.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 0.0F}};
    for (int i = 0; i < CONSTRAINT_TRACKS; i++) {
        const uint32_t at = WEIGHT_VALUES + i * sizeof weights[i];
        const test_track_t track = {
            owner, fields[i], component, ANIM_VALUE_FLOAT, TIMES, at, KEYS, ANIM_LINEAR,
        };
        test_track_row(clip, count + i, &track);
        test_pack_put_floats(clip + at, weights[i], KEYS);
    }
}

static uint8_t*
weighted_fixture(test_pack_t* writer, anim_wrap_t wrap, bool blocks) {
    uint8_t* entry = fixture(writer, 1, 1, 0, 0, false, ANCHOR_NAME, POSITION, DURATION);
    const scene_asset_position_constraint_t row = {.entity = CAMERA, .source_count = 2, .weight = 1.0F};
    const scene_asset_constraint_source_t first = {.target = name_hash("red"), .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &first, sizeof first);
    part_row(entry, SCENE_ASSET_TIMELINE)[offsetof(scene_asset_timeline_t, wrap)] = wrap;
    if (blocks) {
        put_blocks(writer, "gallop");
    }
    put_constraint_weights(writer, blocks ? BLOCK_TRACKS : TRACKS, CAMERA_NAME, ANIM_COMPONENT_POSITION_CONSTRAINT);
    return entry;
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
        weighted_fixture(&writer, wraps[mode], true);
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
        fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
test_constraint_source_curves_match_play_and_seek(void) {
    test_pack_t writer;
    weighted_fixture(&writer, ANIM_LOOP, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    const scene_t* b = columns(sought);
    const vec3f_t own = a->asset.transforms[CAMERA].position;
    uint32_t previous = 0;
    timeline_play(played);
    for (uint32_t now = 0; now <= DURATION + DURATION / 2; now += SAMPLE_FRAME_MS) {
        scene_animation_step(a, now - previous);
        timeline_seek(sought, now);
        previous = now;
        const float t = (float)(now % DURATION) / DURATION;
        const vec3f_t first = {1.0F + 3.0F * t, 0, 0};
        const vec3f_t second = {3.0F + 3.0F * t, 0, -10.0F};
        const vec3f_t expected = vec3f_lerp(own, vec3f_lerp(first, second, t), 1.0F - t);
        const transformf_t x = scene_row_world(a->root, CAMERA);
        const transformf_t y = scene_row_world(b->root, CAMERA);
        TEST_ASSERT_FLOAT_WITHIN(0.00001F, expected.x, x.position.x);
        TEST_ASSERT_FLOAT_WITHIN(0.00001F, expected.z, x.position.z);
        TEST_ASSERT_EQUAL_MEMORY(&x.position, &y.position, sizeof x.position);
        TEST_ASSERT_EQUAL_MEMORY(&x.rotation, &y.rotation, sizeof x.rotation);
        TEST_ASSERT_EQUAL_MEMORY(&x.scale, &y.scale, sizeof x.scale);
    }
    const vec3f_t at_wrap = a->locals[CAMERA].position;
    TEST_ASSERT_FALSE(scene_timeline_step(a, DURATION));
    scene_constraint_step(a, false);
    TEST_ASSERT_EQUAL_MEMORY(&at_wrap, &a->locals[CAMERA].position, sizeof at_wrap);
}

static void
test_clamped_constraint_keeps_end_sample(void) {
    test_pack_t writer;
    weighted_fixture(&writer, ANIM_CLAMP, false);
    const float full_weight[] = {1.0F, 1.0F};
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_pack_put_floats(clip + WEIGHT_VALUES + (CONSTRAINT_TRACKS - 1) * sizeof full_weight, full_weight, KEYS);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    const scene_t* b = columns(sought);
    timeline_play(played);
    scene_animation_step(a, DURATION - SAMPLE_FRAME_MS);
    scene_animation_step(a, 2 * SAMPLE_FRAME_MS);
    TEST_ASSERT_EQUAL_UINT32(DURATION, timeline_time_ms(played));
    const vec3f_t end = {6, 0, -10};
    TEST_ASSERT_EQUAL_MEMORY(&end, &a->locals[CAMERA].position, sizeof end);
    (void)scene_bounds(played);
    a->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_PLACED;
    scene_animation_step(a, DURATION);
    TEST_ASSERT_BITS_LOW(SCENE_FLAG_PLACED, a->flags[CAMERA]);
    timeline_seek(sought, 2 * DURATION);
    TEST_ASSERT_EQUAL_MEMORY(&end, &a->locals[CAMERA].position, sizeof end);
    TEST_ASSERT_EQUAL_MEMORY(&end, &b->locals[CAMERA].position, sizeof end);
}

static void
test_pending_position_survives_bounds_and_animator_seek(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, 0, true, "red", POSITION, DURATION);
    scene_asset_position_constraint_t* constraint = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
    constraint->entity = ANCHOR;
    set_first_constraint_source(entry, "marker");
    timeline_keep_first_track(&writer);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    timeline_play(root);
    scene_animation_step(scene, DURATION / 4);
    timeline_pause(root);

    const entity_t target = scene_row_entity(scene, MARKER);
    transformf_t moved = transform_local(target);
    moved.position = (vec3f_t){4, 2, 1};
    transform_set_local(target, moved);
    (void)scene_bounds(root);
    TEST_ASSERT_TRUE(animator_seek(scene_row_entity(scene, CAMERA), ANIMATOR_CURRENT, DURATION / 2));
    TEST_ASSERT_FALSE(vec3f_equal(moved.position, scene->locals[ANCHOR].position));
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    TEST_ASSERT_EQUAL_MEMORY(&moved.position, &scene->locals[ANCHOR].position, sizeof moved.position);
}

static void
test_position_consumes_placement_without_an_animator(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, 0, true, CAMERA_NAME, POSITION, DURATION);
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_ANIMATORS * sizeof(uint16_t), 0);
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_CLIPS * sizeof(uint16_t), 0);
    set_first_constraint_source(entry, ANCHOR_NAME);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_t* scene = columns(load(&pack));
    TEST_ASSERT_EQUAL_INT(0, scene->animator_count);

    const entity_t target = scene_row_entity(scene, ANCHOR);
    transformf_t moved = transform_local(target);
    moved.position = (vec3f_t){4, 2, 1};
    transform_set_local(target, moved);
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    TEST_ASSERT_EQUAL_MEMORY(&moved.position, &scene->locals[CAMERA].position, sizeof moved.position);
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    TEST_ASSERT_BITS_LOW(SCENE_FLAG_PENDING_PLACEMENT, scene->flags[ANCHOR]);
    TEST_ASSERT_BITS_LOW(SCENE_FLAG_PENDING_PLACEMENT, scene->flags[CAMERA]);
    const vec3f_t unwritten = vec3f_zero();
    transformf_set_position(&scene->locals[CAMERA], unwritten);
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    TEST_ASSERT_EQUAL_MEMORY(&unwritten, &scene->locals[CAMERA].position, sizeof unwritten);
}

static void
assert_position_blend(float own, float target, float weight, float expected) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    scene_asset_position_constraint_t* constraint = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
    constraint->weight = weight;
    set_first_constraint_source(entry, ANCHOR_NAME);
    scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
    transforms[CAMERA].position = (vec3f_t){own, 0, 0};
    transforms[ANCHOR].position = (vec3f_t){target, 0, 0};
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const scene_t* scene = columns(load(&pack));
    TEST_ASSERT_EQUAL_FLOAT(expected, scene->locals[CAMERA].position.x);
}

static void
test_position_blend_at_full_weight_returns_exact_target(void) {
    assert_position_blend(0x1p25F, 1.0F, 1.0F, 1.0F);
}

static void
test_position_blend_handles_large_opposite_positions(void) {
    assert_position_blend(2e38F, -2e38F, 0.5F, 0.0F);
}

static void
test_constraint_weight_zero_restores_the_own_position(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_position_constraint_t row = {.entity = ANCHOR, .source_count = 2, .weight = 1.0F};
    const scene_asset_constraint_source_t second = {name_hash("marker"), 0.0F};
    memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES) + sizeof second, &second, sizeof second);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
    scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
    transforms[ANCHOR].position = (vec3f_t){1, 2, 3};
    put_constraint_weights(&writer, TRACKS, ANCHOR_NAME, ANIM_COMPONENT_POSITION_CONSTRAINT);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    const vec3f_t own = scene->asset.transforms[ANCHOR].position;
    TEST_ASSERT_FALSE(vec3f_equal(own, scene->locals[ANCHOR].position));
    timeline_seek(root, DURATION);
    TEST_ASSERT_EQUAL_MEMORY(&own, &scene->locals[ANCHOR].position, sizeof own);
    const vec3f_t world = scene_row_world(scene->root, ANCHOR).position;
    const vec3f_t parent = scene_row_world(scene->root, SUBJECT).position;
    const vec3f_t expected = vec3f_add(parent, vec3f_scale(own, scene->locals[SUBJECT].scale.x));
    TEST_ASSERT_EQUAL_MEMORY(&expected, &world, sizeof world);
}

static void
test_constraint_normalises_source_weights(void) {
    const float weights[] = {0.0F, 0.2F};
    for (size_t i = 0; i < sizeof weights / sizeof *weights; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
        row->source_count = 2;
        const scene_asset_constraint_source_t sources[] = {{name_hash(ANCHOR_NAME), weights[i]},
                                                           {name_hash("red"), weights[i]}};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[ANCHOR].position = vec3f_unit_x();
        transforms[SUBJECT].position = (vec3f_t){3, 0, 0};
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const scene_t* scene = columns(load(&pack));
        const vec3f_t expected = i == 0 ? transforms[CAMERA].position : (vec3f_t){2, 0, 0};
        TEST_ASSERT_EQUAL_MEMORY(&expected, &scene->locals[CAMERA].position, sizeof expected);
        release_fixture();
    }
}

static void
test_position_uses_ordered_sources_and_clamps_weight(void) {
    test_pack_t writer;
    uint8_t* entry =
        fixture_with_sources(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION, SCENE_CONSTRAINT_SOURCES_MAX);
    scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
    row->source_count = SCENE_CONSTRAINT_SOURCES_MAX;
    const scene_asset_constraint_source_t sources[] = {
        {name_hash(ANCHOR_NAME), 0.25F},
        {name_hash("red"), 0.75F},
        {name_hash("marker"), 0.25F},
        {name_hash(ANCHOR_NAME), 0.25F},
    };
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
    scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
    transforms[ANCHOR].position = (vec3f_t){2, 0, 0};
    transforms[SUBJECT].position = (vec3f_t){6, 0, 0};
    transforms[MARKER].position = (vec3f_t){10, 0, 0};
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_t* scene = columns(load(&pack));
    TEST_ASSERT_FLOAT_WITHIN(0.00001F, 16.0F / 3.0F, scene->locals[CAMERA].position.x);

    scene_constraint_weights_t* weights = &scene->positions[0].inputs.weights;
    weights->weight2 = weights->weight3 = 0.0F;
    weights->weight = 2.0F;
    scene_constraint_step(scene, true);
    TEST_ASSERT_EQUAL_FLOAT(5.0F, scene->locals[CAMERA].position.x);
    weights->weight0 = 0.75F;
    weights->weight1 = 0.25F;
    scene_constraint_step(scene, true);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, scene->locals[CAMERA].position.x);
    weights->weight = -1.0F;
    scene_constraint_step(scene, true);
    TEST_ASSERT_EQUAL_FLOAT(transforms[CAMERA].position.x, scene->locals[CAMERA].position.x);
}

static void
assert_constraint_weight_refused(bool aim, bool property) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, !property, !aim, aim, 0, false, ANCHOR_NAME, POSITION, DURATION);
    set_first_constraint_source(entry, ANCHOR_NAME);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_tracks_header(clip, 1, DURATION);
    const uint32_t component = aim ? ANIM_COMPONENT_AIM_CONSTRAINT : ANIM_COMPONENT_POSITION_CONSTRAINT;
    const test_track_t track = {
        CAMERA_NAME, "weight0", component, ANIM_VALUE_FLOAT, TIMES, WEIGHT_VALUES, KEYS, ANIM_LINEAR,
    };
    const float weights[] = {1.0F, property ? 0.0F : 1.25F};
    test_track_row(clip, 0, &track);
    test_pack_put_floats(clip + WEIGHT_VALUES, weights, KEYS);

    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_failure_t failure;
    if (property) {
        scene_t* scene = columns(root);
        const char* const clips[] = {CLIP};
        TEST_ASSERT_FALSE(animator_set_clips(scene_row_entity(scene, CAMERA), NULL, clips, 1, &failure));
    } else {
        TEST_ASSERT_TRUE(scene_failure(root, &failure));
    }
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
    TEST_ASSERT_EQUAL_STRING(aim ? "camera:CAIM.weight0" : "camera:CPOS.weight0", failure.what);
}

static void
test_position_weight_key_outside_range_is_refused(void) {
    assert_constraint_weight_refused(false, false);
}

static void
test_aim_weight_key_outside_range_is_refused(void) {
    assert_constraint_weight_refused(true, false);
}

static void
test_runtime_animator_cannot_write_position_weight(void) {
    assert_constraint_weight_refused(false, true);
}

static void
test_runtime_animator_cannot_write_aim_weight(void) {
    assert_constraint_weight_refused(true, true);
}

static void
test_constraint_refuses_a_missing_source_track(void) {
    test_pack_t writer;
    uint8_t* entry = weighted_fixture(&writer, ANIM_CLAMP, false);
    part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS)[SCENE_ASSET_POSITION_CONSTRAINT_AT_SOURCE_COUNT] = 1;
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_STRING("camera:CPOS.weight1", failure.what);
}

static void
assert_constraint_vector(vec3f_t expected, vec3f_t actual) {
    const float tolerance = 0.00001F;
    TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, expected.z, actual.z);
}

static void
test_aim_coincident_weighted_sources_keep_own_rotation(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    scene_asset_aim_constraint_t* aim = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
    aim->source_count = 2;
    const scene_asset_constraint_source_t sources[] = {{name_hash("red"), 0.1F}, {name_hash(ANCHOR_NAME), 0.2F}};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
    scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
    transforms[CAMERA].position = transforms[SUBJECT].position = transforms[ANCHOR].position = (vec3f_t){1e8F, 0, 0};
    transforms[CAMERA].linear = (mat3f_t){{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const scene_t* scene = columns(load(&pack));
    const quatf_t own = scene->aims[0].own;
    TEST_ASSERT_EQUAL_MEMORY(&own, &scene->locals[CAMERA].rotation, sizeof own);
}

static void
test_position_collapsed_parent_keeps_own_local(void) {
    const float scales[] = {FLT_MIN, 0.0F};
    for (size_t i = 0; i < sizeof scales / sizeof *scales; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
        row->entity = MARKER;
        set_first_constraint_source(entry, ANCHOR_NAME);
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + MARKER * sizeof(uint16_t), SUBJECT);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[SUBJECT].position = vec3f_zero();
        transforms[ANCHOR].position = (vec3f_t){8, 0, 0};
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        const scene_t* scene = columns(root);
        transformf_t parent = scene->locals[SUBJECT];
        parent.scale = (vec3f_t){scales[i], scales[i], scales[i]};
        transform_set_local(scene_row_entity(scene, SUBJECT), parent);
        scene_animation_step(columns(root), 0);
        const vec3f_t own = transforms[MARKER].position;
        TEST_ASSERT_EQUAL_MEMORY(&own, &scene->locals[MARKER].position, sizeof own);
        release_fixture();
    }
}

static void
test_position_source_app_move_without_timeline(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    scene_asset_position_constraint_t* position = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
    position->source_count = 2;
    const scene_asset_constraint_source_t first = {name_hash("red"), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &first, sizeof first);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const entity_t source = scene_row_entity(scene, SUBJECT);
    transformf_t moved = transform_local(source);
    moved.position = (vec3f_t){4, 2, 1};
    transform_set_local(source, moved);
    (void)scene_bounds(root);
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_MEMORY(&moved.position, &scene->locals[CAMERA].position, sizeof moved.position);

    scene->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_PLACED;
    scene_constraint_step(scene, true);
    TEST_ASSERT_BITS_LOW(SCENE_FLAG_PLACED, scene->flags[CAMERA]);
    scene_animation_step(scene, 0);
    TEST_ASSERT_BITS_LOW(SCENE_FLAG_PLACED, scene->flags[CAMERA]);
    moved.position = vec3f_zero();
    transform_set_local(scene_row_entity(scene, ANCHOR), moved);
    scene_animation_step(scene, 0);
    TEST_ASSERT_BITS_LOW(SCENE_FLAG_PLACED, scene->flags[CAMERA]);
}

static void
test_position_source_cannot_own_another_constraint(void) {
    enum { SOURCE_DRIVEN, OWNER_PARENT_DRIVEN, DUPLICATE };

    for (int test = SOURCE_DRIVEN; test <= DUPLICATE; test++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 2, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_position_constraint_t* rows = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
        rows[1] =
            (scene_asset_position_constraint_t){.entity = ANCHOR, .source_first = 1, .source_count = 1, .weight = 1.0F};
        scene_asset_constraint_source_t sources[] = {{name_hash(ANCHOR_NAME), 1.0F}, {name_hash("red"), 1.0F}};
        if (test == OWNER_PARENT_DRIVEN) {
            rows[0].entity = ANCHOR;
            rows[1].entity = SUBJECT;
            sources[0].target = name_hash(CAMERA_NAME);
            sources[1].target = name_hash("marker");
            test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
        } else if (test == DUPLICATE) {
            rows[1].entity = CAMERA;
        }
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        const char* expected = test == SOURCE_DRIVEN         ? "anchor:TRNS.position"
                               : test == OWNER_PARENT_DRIVEN ? "red:TRNS.position"
                                                             : SCENE_TEST_TIMELINE_ID;
        TEST_ASSERT_EQUAL_STRING(expected, failure.what);
        release_fixture();
    }
}

static void
test_aim_animated_weight_and_zero_source_sum(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 1, 0, false, ANCHOR_NAME, POSITION, DURATION);
    scene_asset_aim_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
    row->entity = MARKER;
    row->source_count = 2;
    const scene_asset_constraint_source_t sources[] = {{name_hash(ANCHOR_NAME), 1.0F}, {name_hash(CAMERA_NAME), 0.0F}};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
    put_constraint_weights(&writer, TRACKS, "marker", ANIM_COMPONENT_AIM_CONSTRAINT);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const float source_weights[] = {1, 0, 0, 0};
    test_pack_put_floats(clip + WEIGHT_VALUES, source_weights, sizeof source_weights / sizeof *source_weights);
    const test_track_t source_curve = {
        "marker",   "weight0",   ANIM_COMPONENT_AIM_CONSTRAINT, ANIM_VALUE_FLOAT, SPEED_TIMES, SPEED_VALUES,
        SPEED_KEYS, ANIM_LINEAR,
    };
    const uint32_t zero_ms = 3 * DURATION / 4;
    const float zero_at = (float)zero_ms / DURATION;
    const float half_frame = (float)SAMPLE_FRAME_MS / (2.0F * DURATION);
    const float source_times[] = {0, zero_at - half_frame, zero_at + half_frame, 1};
    const float source_fade[] = {1, 0, 0, 1};
    test_track_row(clip, TRACKS, &source_curve);
    test_pack_put_floats(clip + SPEED_TIMES, source_times, SPEED_KEYS);
    test_pack_put_floats(clip + SPEED_VALUES, source_fade, SPEED_KEYS);
    const float positions[] = {3, 0, 0, 3, 0, 0};
    test_pack_put_floats(clip + CAMERA_VALUES, positions, sizeof positions / sizeof *positions);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const quatf_t own = quatf_identity();
    const quatf_t full = quatf_from_axis_angle(vec3f_up(), MATH_PI / 2.0F);
    timeline_play(root);
    scene_animation_step(scene, DURATION / 2);
    const quatf_t half = quatf_nlerp(own, full, 0.5F);
    const vec3f_t forward = vec3f_forward();
    assert_constraint_vector(quatf_rotate(half, forward), quatf_rotate(scene->locals[MARKER].rotation, forward));

    scene->aims[0].inputs.weights.weight = 2.0F;
    scene_constraint_step(scene, true);
    assert_constraint_vector(vec3f_unit_x(), quatf_rotate(scene->locals[MARKER].rotation, forward));
    scene->aims[0].inputs.weights.weight0 = -1.0F;
    scene->aims[0].inputs.weights.weight1 = 1.0F;
    scene_constraint_step(scene, true);
    assert_constraint_vector(vec3f_back(), quatf_rotate(scene->locals[MARKER].rotation, forward));
    scene->aims[0].inputs.weights.weight0 = 1.0F;
    scene->aims[0].inputs.weights.weight = 0.0F;
    scene_constraint_step(scene, true);
    TEST_ASSERT_EQUAL_MEMORY(&own, &scene->locals[MARKER].rotation, sizeof own);

    scene_animation_step(scene, zero_ms - DURATION / 2 - SAMPLE_FRAME_MS);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&own, &scene->locals[MARKER].rotation, sizeof own));
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    TEST_ASSERT_FLOAT_WITHIN(0.00001F, 1.0F - zero_at, scene->aims[0].inputs.weights.weight);
    const quatf_t played = scene->locals[MARKER].rotation;
    TEST_ASSERT_EQUAL_MEMORY(&own, &played, sizeof own);
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&own, &scene->locals[MARKER].rotation, sizeof own));
    timeline_seek(root, zero_ms);
    TEST_ASSERT_EQUAL_MEMORY(&played, &scene->locals[MARKER].rotation, sizeof played);
    scene_animation_step(scene, DURATION);
    TEST_ASSERT_EQUAL_MEMORY(&own, &scene->locals[MARKER].rotation, sizeof own);
}

static void
test_aim_weight_track_requires_an_existing_source(void) {
    for (int count = 0; count <= 2; count++) {
        test_pack_t writer;
        uint8_t* entry =
            fixture(&writer, 1, 0, count != 0, 0, false, count != 0 ? ANCHOR_NAME : CAMERA_NAME, POSITION, DURATION);
        if (count != 0) {
            scene_asset_aim_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
            row->entity = MARKER;
            row->source_count = count;
        }
        put_constraint_weights(&writer, TRACKS, count != 0 ? "marker" : CAMERA_NAME, ANIM_COMPONENT_AIM_CONSTRAINT);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        if (count == 2) {
            (void)columns(root);
        } else {
            scene_failure_t failure;
            TEST_ASSERT_TRUE(scene_failure(root, &failure));
            TEST_ASSERT_EQUAL_STRING(count == 0 ? "camera:CAIM.weight0" : "marker:CAIM.weight1", failure.what);
        }
        release_fixture();
    }
}

static void
test_parent_frame_maps_position_and_aim_locals(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    scene_asset_position_constraint_t* position = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
    scene_asset_aim_constraint_t* aim = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
    position->entity = aim->entity = MARKER;
    aim->source_first = 1;
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + MARKER * sizeof(uint16_t), SUBJECT);
    scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
    const float sx = 2.0F, sy = 2.0001F, sz = 2.00015F;
    transforms[SUBJECT].linear = (mat3f_t){{{0, 0, sz}, {0, sy, 0}, {-sx, 0, 0}}};
    transforms[CAMERA].position = (vec3f_t){sz, sy, -sx};
    transforms[ANCHOR].position = vec3f_add(transforms[CAMERA].position, vec3f_forward());
    ((scene_asset_constraint_source_t*)part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES))[1].weight = 1.0F;
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_t* scene = columns(load(&pack));
    assert_constraint_vector(vec3f_one(), scene->locals[MARKER].position);
    transformf_t world = scene_row_world(scene->root, MARKER);
    assert_constraint_vector(vec3f_forward(), quatf_rotate(world.rotation, vec3f_forward()));

    const entity_t target = scene_row_entity(scene, ANCHOR);
    transformf_t parallel = transform_local(target);
    parallel.position = vec3f_add(transforms[CAMERA].position, vec3f_up());
    transform_set_local(target, parallel);
    scene_animation_step(scene, 0);
    world = scene_row_world(scene->root, MARKER);
    assert_constraint_vector(vec3f_up(), quatf_rotate(world.rotation, vec3f_forward()));
}

static void
test_position_constraint_binding_modes(void) {
    const rig_bind_t modes[] = {RIG_BIND_WORLD, RIG_BIND_TARGET, RIG_BIND_TARGET_YAW};
    const vec3f_t offset = vec3f_one();
    const quatf_t yaw = quatf_from_axis_angle(vec3f_up(), MATH_PI / 2.0F);
    const quatf_t pitch = quatf_from_axis_angle(vec3f_unit_x(), MATH_PI / 4.0F);
    const quatf_t roll = quatf_from_axis_angle(vec3f_forward(), MATH_PI / 4.0F);
    const quatf_t tilt = quatf_mul(pitch, roll);
    const quatf_t end = quatf_mul(yaw, tilt);
    const float turns[] = {tilt.x, tilt.y, tilt.z, tilt.w, end.x, end.y, end.z, end.w};

    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, 0, false, ANCHOR_NAME, POSITION, DURATION);
        scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
        row->binding_mode = modes[i];
        row->translation_offset = offset;
        set_first_constraint_source(entry, ANCHOR_NAME);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const test_track_t turn = {
            ANCHOR_NAME, "rotation", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, ROTATION_VALUES,
            KEYS,        ANIM_LINEAR};
        test_track_row(clip, 4, &turn);
        test_pack_put_floats(clip + ROTATION_VALUES, turns, sizeof turns / sizeof *turns);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t played = load(&pack), sought = load(&pack);
        scene_t* live = columns(played);
        const scene_t* fixed = columns(sought);
        timeline_play(played);
        uint32_t previous = 0;
        for (uint32_t ms = 0; ms <= DURATION; ms += DURATION / 4) {
            scene_animation_step(live, ms - previous);
            timeline_seek(sought, ms);
            previous = ms;
            const float t = (float)ms / DURATION;
            const quatf_t level = quatf_from_axis_angle(vec3f_up(), t * MATH_PI / 2.0F);
            const quatf_t frame = modes[i] == RIG_BIND_TARGET       ? quatf_mul(level, tilt)
                                  : modes[i] == RIG_BIND_TARGET_YAW ? level
                                                                    : quatf_identity();
            const vec3f_t expected = vec3f_add(fixed->locals[ANCHOR].position, quatf_rotate(frame, offset));
            const vec3f_t shown = live->locals[CAMERA].position;
            assert_constraint_vector(expected, shown);
            TEST_ASSERT_EQUAL_MEMORY(&shown, &fixed->locals[CAMERA].position, sizeof shown);
        }
        release_fixture();
    }
}

static void
test_aim_updates_only_for_placed_input_paths(void) {
    static const struct {
        uint8_t mode;
        bool marker_is_source;
    } cases[] = {
        {SCENE_ASSET_WORLD_UP_SCENE_UP, true},   {SCENE_ASSET_WORLD_UP_SCENE_UP, false},
        {SCENE_ASSET_WORLD_UP_VECTOR, true},     {SCENE_ASSET_WORLD_UP_VECTOR, false},
        {SCENE_ASSET_WORLD_UP_OBJECT_UP, false}, {SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP, false},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_aim_constraint_t* aim = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
        aim->world_up_type = cases[i].mode;
        const bool uses_object =
            cases[i].mode == SCENE_ASSET_WORLD_UP_OBJECT_UP || cases[i].mode == SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP;
        aim->world_up_object = uses_object ? MARKER : SCENE_ASSET_WORLD_UP_OBJECT_NONE;
        aim->source_count = cases[i].marker_is_source ? 2 : 1;
        const scene_asset_constraint_source_t sources[] = {{name_hash(ANCHOR_NAME), 1.0F}, {name_hash("marker"), 0.0F}};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[CAMERA].position = vec3f_zero();
        transforms[SUBJECT].position = vec3f_zero();
        transforms[ANCHOR].position = vec3f_forward();
        transforms[MARKER].position = vec3f_up();
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        scene_t* scene = columns(root);

        const entity_t target = scene_row_entity(scene, ANCHOR);
        transformf_t moved = transform_local(target);
        moved.position = vec3f_unit_x();
        transform_set_local(target, moved);
        scene_animation_step(scene, 0);
        assert_constraint_vector(vec3f_unit_x(), quatf_rotate(scene->locals[CAMERA].rotation, vec3f_forward()));

        const entity_t parent = scene_row_entity(scene, SUBJECT);
        moved = transform_local(parent);
        moved.position = (vec3f_t){-1, 0, 1};
        transform_set_local(parent, moved);
        scene_animation_step(scene, 0);
        assert_constraint_vector(vec3f_forward(), quatf_rotate(scene->locals[CAMERA].rotation, vec3f_forward()));

        (void)scene_bounds(root);
        scene->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_PLACED;
        const quatf_t unwritten = quatf_from_axis_angle(vec3f_up(), MATH_PI / 4.0F);
        if (!uses_object) {
            transformf_set_rotation(&scene->locals[CAMERA], unwritten);
        }
        const entity_t up = scene_row_entity(scene, MARKER);
        moved = transform_local(up);
        moved.position = vec3f_unit_x();
        transformf_set_rotation(&moved, quatf_from_axis_angle(vec3f_forward(), -MATH_PI / 2.0F));
        transform_set_local(up, moved);
        scene_animation_step(scene, 0);
        if (uses_object) {
            assert_constraint_vector(vec3f_unit_x(), quatf_rotate(scene->locals[CAMERA].rotation, vec3f_up()));
            TEST_ASSERT_BITS_HIGH(SCENE_FLAG_DIRTY, scene->flags[CAMERA]);
        } else {
            TEST_ASSERT_EQUAL_MEMORY(&unwritten, &scene->locals[CAMERA].rotation, sizeof unwritten);
            TEST_ASSERT_BITS_LOW(SCENE_FLAG_PLACED, scene->flags[CAMERA]);
        }
        release_fixture();
    }
}

static void
test_aim_cutoff_uses_relative_direction(void) {
    static const struct {
        float origin, separation;
        bool aims;
    } cases[] = {
        {1e8F, 1.0F, true}, {1e8F, 0.0F, false}, {1e4F, FLT_EPSILON / 2.0F, false}, {1e4F, FLT_EPSILON * 2.0F, true}};

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        set_first_constraint_source(entry, ANCHOR_NAME);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[CAMERA].position = (vec3f_t){cases[i].origin, 0, 0};
        transforms[ANCHOR].position = (vec3f_t){cases[i].origin, cases[i].separation, 0};
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const scene_t* scene = columns(load(&pack));
        const vec3f_t expected = cases[i].aims ? vec3f_up() : vec3f_forward();
        assert_constraint_vector(expected, quatf_rotate(scene->locals[CAMERA].rotation, vec3f_forward()));
        release_fixture();
    }
}

static void
test_aim_only_requires_world_up_vector_in_modes_that_read_it(void) {
    static const struct {
        uint8_t mode;
        uint16_t reference;
        bool reads_vector;
    } cases[] = {
        {SCENE_ASSET_WORLD_UP_SCENE_UP, SCENE_ASSET_WORLD_UP_OBJECT_NONE, false},
        {SCENE_ASSET_WORLD_UP_NONE, SCENE_ASSET_WORLD_UP_OBJECT_NONE, false},
        {SCENE_ASSET_WORLD_UP_OBJECT_UP, MARKER, false},
        {SCENE_ASSET_WORLD_UP_VECTOR, SCENE_ASSET_WORLD_UP_OBJECT_NONE, true},
        {SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP, MARKER, true},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_aim_constraint_t* aim = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
        aim->world_up_type = cases[i].mode;
        aim->world_up_object = cases[i].reference;
        aim->world_up_vector = vec3f_zero();
        set_first_constraint_source(entry, ANCHOR_NAME);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_EQUAL_INT(cases[i].reads_vector, scene_failure(load(&pack), &failure));
        release_fixture();
    }
}

static void
test_aim_parallel_or_coincident_up_stays_finite(void) {
    for (uint8_t mode = SCENE_ASSET_WORLD_UP_SCENE_UP; mode <= SCENE_ASSET_WORLD_UP_NONE; mode++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_aim_constraint_t* aim = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
        aim->world_up_type = mode;
        if (mode == SCENE_ASSET_WORLD_UP_OBJECT_UP || mode == SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP) {
            aim->world_up_object = MARKER;
        }
        set_first_constraint_source(entry, ANCHOR_NAME);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[CAMERA].position = vec3f_zero();
        transforms[ANCHOR].position = vec3f_up();
        transforms[MARKER].position = vec3f_up();
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_t* scene = columns(load(&pack));
        assert_constraint_vector(vec3f_up(), quatf_rotate(scene->locals[CAMERA].rotation, vec3f_forward()));
        if (mode == SCENE_ASSET_WORLD_UP_OBJECT_UP) {
            const entity_t up = scene_row_entity(scene, MARKER);
            transformf_t moved = transform_local(up);
            moved.position = vec3f_zero();
            transform_set_local(up, moved);
            scene_animation_step(scene, 0);
            assert_constraint_vector(vec3f_up(), quatf_rotate(scene->locals[CAMERA].rotation, vec3f_forward()));
        }
        release_fixture();
    }
}

static void
assert_aim_rotation_offset_validation(float tolerance_multiple, bool refused) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 0, 1, 0, false, CAMERA_NAME, POSITION, DURATION);
    scene_asset_aim_constraint_t* aim = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
    aim->rotation_offset.w = sqrtf(1.0F + tolerance_multiple * SCENE_ASSET_AIM_ROTATION_TOLERANCE);
    set_first_constraint_source(entry, ANCHOR_NAME);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_EQUAL_INT(refused, scene_failure(load(&pack), &failure));
    if (refused) {
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
    }
    release_fixture();
}

static void
test_aim_rotation_offset_unit_tolerance(void) {
    assert_aim_rotation_offset_validation(0.5F, false);
    assert_aim_rotation_offset_validation(2.0F, true);
}

static void
test_aim_constraint_world_up_and_position_order(void) {
    const uint8_t modes[] = {SCENE_ASSET_WORLD_UP_SCENE_UP, SCENE_ASSET_WORLD_UP_OBJECT_UP,
                             SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP, SCENE_ASSET_WORLD_UP_VECTOR,
                             SCENE_ASSET_WORLD_UP_NONE};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 1, 0, false, "marker", POSITION, DURATION);
        const uint8_t mode = modes[i];
        const bool object = mode == SCENE_ASSET_WORLD_UP_OBJECT_UP || mode == SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP;
        const vec3f_t aim_axis = vec3f_unit_x(), up_axis = vec3f_forward();
        const quatf_t offset = quatf_from_axis_angle(aim_axis, MATH_PI / 4.0F);
        const scene_asset_aim_constraint_t aim = {
            .entity = CAMERA,
            .world_up_type = mode,
            .source_count = 1,
            .source_first = 1,
            .world_up_object = object ? MARKER : SCENE_ASSET_WORLD_UP_OBJECT_NONE,
            .weight = 1.0F,
            .aim_vector = aim_axis,
            .up_vector = up_axis,
            .world_up_vector = {0, 0, 1},
            .rotation_offset = offset,
        };
        const scene_asset_position_constraint_t position = {
            .entity = CAMERA,
            .source_count = 1,
            .weight = 1.0F,
            .translation_offset = {0, 2, 2},
        };
        const scene_asset_constraint_source_t sources[] = {{name_hash("red"), 1.0F}, {name_hash(ANCHOR_NAME), 1.0F}};
        memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &position, sizeof position);
        memcpy(part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS), &aim, sizeof aim);
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[ANCHOR].position = (vec3f_t){2, 0, 4};
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
        transforms[CAMERA].linear = (mat3f_t){{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};
        const quatf_t own = quatf_from_axis_angle(aim_axis, MATH_PI / 2.0F);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const float up_positions[] = {0, 3, -4, 2, 5, -1};
        test_pack_put_floats(clip + CAMERA_VALUES, up_positions, sizeof up_positions / sizeof *up_positions);
        const test_track_t turn = {
            "marker", "rotation", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, ROTATION_VALUES, KEYS, ANIM_LINEAR,
        };
        test_track_row(clip, 4, &turn);
        test_track_t target_turn = turn;
        target_turn.path = "red";
        test_track_row(clip, 2, &target_turn);
        const float turns[] = {0, 0, 0, 1, own.x, own.y, own.z, own.w};
        test_pack_put_floats(clip + ROTATION_VALUES, turns, sizeof turns / sizeof *turns);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack), played = load(&pack);
        const scene_t* scene = columns(root);
        scene_t* live = columns(played);
        timeline_play(played);
        const uint32_t samples[] = {0, DURATION / 4, DURATION / 2, DURATION};
        uint32_t previous = 0;
        for (size_t s = 0; s < sizeof samples / sizeof *samples; s++) {
            timeline_seek(root, samples[s]);
            scene_animation_step(live, samples[s] - previous);
            previous = samples[s];
            const transformf_t* at = &scene->locals[CAMERA];
            const transformf_t* up_object = &scene->locals[MARKER];
            const transformf_t* follow_source = &scene->locals[SUBJECT];
            const vec3f_t follow = follow_source->position;
            const vec3f_t aim_point =
                vec3f_add(follow, quatf_rotate(follow_source->rotation,
                                               vec3f_scale(scene->locals[ANCHOR].position, follow_source->scale.x)));
            assert_constraint_vector(vec3f_add(follow, position.translation_offset), at->position);
            TEST_ASSERT_EQUAL_MEMORY(&at->rotation, &live->locals[CAMERA].rotation, sizeof at->rotation);
            const vec3f_t direction = vec3f_normalize(vec3f_sub(aim_point, at->position));
            const vec3f_t swapped = vec3f_normalize(vec3f_sub(aim_point, transforms[CAMERA].position));
            TEST_ASSERT_TRUE(vec3f_dot(direction, swapped) < 0.99F);
            const quatf_t aimed = quatf_mul(at->rotation, quatf_conjugate(offset));
            assert_constraint_vector(direction, quatf_rotate(aimed, aim_axis));
            vec3f_t up = mode == SCENE_ASSET_WORLD_UP_SCENE_UP ? vec3f_up() : vec3f_forward();
            if (mode == SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP) {
                up = quatf_rotate(up_object->rotation, up);
            } else if (mode == SCENE_ASSET_WORLD_UP_OBJECT_UP) {
                up = vec3f_sub(up_object->position, at->position);
            } else if (mode == SCENE_ASSET_WORLD_UP_NONE) {
                const quatf_t swing = quatf_from_to(aim_axis, direction);
                up = quatf_rotate(quatf_mul(swing, own), up_axis);
            }
            up = vec3f_normalize(vec3f_sub(up, vec3f_scale(direction, vec3f_dot(up, direction))));
            assert_constraint_vector(up, quatf_rotate(aimed, up_axis));
        }
        release_fixture();
    }
}

static void
test_constraints_refuse_claimed_writers_and_driven_inputs(void) {
    enum { TIMELINE_WRITER, PROPERTY_WRITER, SOURCE_ANCESTOR, UP_ANCESTOR, SOURCE_HINGE, UP_HINGE };

    for (int test = TIMELINE_WRITER; test <= UP_HINGE; test++) {
        const bool up = test == UP_ANCESTOR || test == UP_HINGE;
        const bool hinge = test >= SOURCE_HINGE;
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, test == TIMELINE_WRITER, !up, up, hinge, test != TIMELINE_WRITER && !hinge,
                                 CAMERA_NAME, POSITION, DURATION);
        const bool writer_conflict = test <= PROPERTY_WRITER;
        if (test == SOURCE_HINGE) {
            ((scene_asset_hinge_t*)part_row(entry, SCENE_ASSET_HINGES))->entity = ANCHOR;
        }
        const scene_asset_constraint_source_t source = {
            name_hash(up                ? "marker"
                      : writer_conflict ? "red"
                                        : ANCHOR_NAME),
            1.0F,
        };
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        if (up) {
            scene_asset_aim_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
            row->entity = SUBJECT;
            row->world_up_type = SCENE_ASSET_WORLD_UP_OBJECT_UP;
            row->world_up_object = hinge ? CAMERA : ANCHOR;
        } else if (!writer_conflict) {
            scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
            row->entity = SUBJECT;
        }
        if (!writer_conflict && !hinge) {
            test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), CAMERA);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        const char* expected[] = {"camera:TRNS.position", ".:TRNS.position",   ".:TRNS.position",
                                  ".:TRNS.position",      "anchor:HNGE.angle", "camera:HNGE.angle"};
        TEST_ASSERT_EQUAL_STRING(expected[test], failure.what);
        release_fixture();
    }
}

static void
test_constraints_claim_only_their_transform_field(void) {
    enum { POSITION_WITH_ROTATION, AIM_WITH_ROTATION, AIM_WITH_POSITION };

    for (int test = POSITION_WITH_ROTATION; test <= AIM_WITH_POSITION; test++) {
        const bool position = test == POSITION_WITH_ROTATION;
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, position, !position, 0, false, position ? ANCHOR_NAME : CAMERA_NAME,
                                 POSITION, DURATION);
        const scene_asset_constraint_source_t source = {name_hash("red"), 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        if (test == AIM_WITH_POSITION) {
            uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
            const test_track_t turn = {
                ANCHOR_NAME, "rotation", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, ROTATION_VALUES,
                KEYS,        ANIM_LINEAR};
            test_track_row(clip, 4, &turn);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        if (test == AIM_WITH_ROTATION) {
            scene_failure_t failure;
            TEST_ASSERT_TRUE(scene_failure(root, &failure));
            TEST_ASSERT_EQUAL_STRING("camera:TRNS.rotation", failure.what);
        } else {
            (void)columns(root);
        }
        release_fixture();
    }
}

static void
test_clock_and_immediate_seek(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    uint8_t* entry = fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, 0);
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
    fixture(&writer, 0, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
            fixture(&writer, 1, 0, 0, cases[i].hinge, cases[i].property, cases[i].path, cases[i].field, DURATION);
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
    uint8_t* entry = fixture(&writer, 2, 2, 2, 2, false, CAMERA_NAME, POSITION, DURATION);
    const asset_view_t view = {.data = entry,
                               .size = (uint32_t)(part_row(entry, SCENE_ASSET_HINGES) - entry)
                                       + 2 * sizeof(scene_asset_hinge_t)};
    scene_asset_t asset;
    const scene_asset_part_t parts[] = {SCENE_ASSET_TIMELINE, SCENE_ASSET_POSITION_CONSTRAINTS,
                                        SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_HINGES};
    const size_t strides[] = {sizeof(scene_asset_timeline_t), sizeof(scene_asset_position_constraint_t),
                              sizeof(scene_asset_aim_constraint_t), sizeof(scene_asset_hinge_t)};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        uint8_t* row = part_row(entry, parts[i]);
        memcpy(row + strides[i], row, strides[i]);
        uint8_t* count = entry + SCENE_ASSET_AT_COUNTS + parts[i] * sizeof(uint16_t);
        test_pack_put16(count, 1);
    }
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(view, &asset));

    const asset_view_t incomplete_header = {.data = entry, .size = SCENE_ASSET_HEADER_SIZE - 1};
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, scene_asset_open(incomplete_header, &asset));
    uint8_t* source = part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES);
    const name_hash_t bad_targets[] = {name_hash("absent"), NAME_HASH_NONE};
    for (size_t i = 0; i < sizeof bad_targets / sizeof *bad_targets; i++) {
        test_pack_put32(source + SCENE_ASSET_CONSTRAINT_SOURCE_AT_TARGET, bad_targets[i]);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
    }
    test_pack_put32(source + SCENE_ASSET_CONSTRAINT_SOURCE_AT_TARGET, name_hash(CAMERA_NAME));
    const float zero = 0.0F;
    test_pack_put_floats(source + SCENE_ASSET_CONSTRAINT_SOURCE_AT_WEIGHT, &zero, 1);
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
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        uint8_t* count = entry + SCENE_ASSET_AT_COUNTS + parts[i] * sizeof(uint16_t);
        test_pack_put16(count, 2);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        test_pack_put16(count, 1);
    }
    uint8_t* timeline = part_row(entry, SCENE_ASSET_TIMELINE);
    uint8_t* position = part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
    uint8_t* aim = part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
    uint8_t* hinges = part_row(entry, SCENE_ASSET_HINGES);

    const struct {
        uint8_t* byte;
        uint8_t bad;
    } bytes[] = {
        {timeline + SCENE_ASSET_TIMELINE_AT_CLIP, 0},
        {timeline + SCENE_ASSET_TIMELINE_AT_WRAP, BAD_CODE},
        {timeline + SCENE_ASSET_TIMELINE_AT_PAD, 1},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_ENTITY, BAD_CODE},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_BINDING_MODE, BAD_CODE},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_PAD, 1},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_SOURCE_COUNT, 0},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_SOURCE_COUNT, SCENE_CONSTRAINT_SOURCES_MAX + 1},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_SOURCE_FIRST, 2},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_TYPE, BAD_CODE},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_TYPE, SCENE_ASSET_WORLD_UP_OBJECT_UP},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_OBJECT, 0},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_SOURCE_FIRST, 2},
        {hinges + SCENE_ASSET_HINGE_AT_ENTITY, BAD_CODE},
        {hinges + SCENE_ASSET_HINGE_AT_PAD, 1},
    };

    for (size_t i = 0; i < sizeof bytes / sizeof *bytes; i++) {
        const uint8_t good = *bytes[i].byte;
        *bytes[i].byte = bytes[i].bad;
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        *bytes[i].byte = good;
    }

    aim[SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_TYPE] = SCENE_ASSET_WORLD_UP_VECTOR;

    static const struct {
        scene_asset_part_t part;
        size_t offset;
        float bad;
    } floats[] = {
        {SCENE_ASSET_POSITION_CONSTRAINTS, SCENE_ASSET_POSITION_CONSTRAINT_AT_TRANSLATION_OFFSET, NAN},
        {SCENE_ASSET_POSITION_CONSTRAINTS, SCENE_ASSET_POSITION_CONSTRAINT_AT_WEIGHT, -0.5F},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_WEIGHT, 1.5F},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_AIM_VECTOR + offsetof(vec3f_t, z), 0.0F},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_UP_VECTOR + offsetof(vec3f_t, y), 0.0F},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_VECTOR, INFINITY},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_ROTATION_OFFSET, NAN},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_ROTATION_OFFSET + offsetof(quatf_t, w), 2.0F},
        {SCENE_ASSET_AIM_CONSTRAINTS, SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_VECTOR + offsetof(vec3f_t, y), 0.0F},
        {SCENE_ASSET_CONSTRAINT_SOURCES, SCENE_ASSET_CONSTRAINT_SOURCE_AT_WEIGHT, 1.5F},
        {SCENE_ASSET_HINGES, SCENE_ASSET_HINGE_AT_AXIS, 1.0F},
        {SCENE_ASSET_HINGES, SCENE_ASSET_HINGE_AT_AXIS, NAN},
        {SCENE_ASSET_HINGES, SCENE_ASSET_HINGE_AT_MIN, 2.0F},
        {SCENE_ASSET_HINGES, SCENE_ASSET_HINGE_AT_MIN, -INFINITY},
        {SCENE_ASSET_HINGES, SCENE_ASSET_HINGE_AT_MAX, INFINITY},
        {SCENE_ASSET_HINGES, SCENE_ASSET_HINGE_AT_ANGLE, NAN},
    };

    for (size_t i = 0; i < sizeof floats / sizeof *floats; i++) {
        uint8_t* at = part_row(entry, floats[i].part) + floats[i].offset;
        float good;
        memcpy(&good, at, sizeof good);
        test_pack_put_floats(at, &floats[i].bad, 1);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        test_pack_put_floats(at, &good, 1);
    }
}

static void
test_timeline_leaves_resting_transforms_clean(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
assert_camera_cut_on_wrap(bool position, bool aim, bool keyed) {
    test_pack_t writer;
    uint8_t* entry =
        fixture(&writer, 1, position, aim, 0, false, keyed ? CAMERA_NAME : ANCHOR_NAME, POSITION, DURATION);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    if (!keyed) {
        set_first_constraint_source(entry, ANCHOR_NAME);
        timeline_keep_first_track(&writer);
    }
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(scene, CAMERA)));
    render_context_t* render = render_context_main();
    render->kept = true;
    timeline_play(root);
    scene_animation_step(scene, DURATION - 1);
    TEST_ASSERT_TRUE(render->kept);
    scene_animation_step(scene, 1);
    TEST_ASSERT_FALSE(render->kept);
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    render->kept = true;
    scene_animation_step(scene, 2 * DURATION);
    TEST_ASSERT_FALSE(render->kept);
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));

    const entity_t other = scene_load_from(&pack, SCENE_TEST_STATIC_ID);
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(columns(other), CAMERA)));
    render->kept = true;
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_TRUE(render->kept);
    scene_animation_step(scene, DURATION);
    TEST_ASSERT_TRUE(render->kept);
}

static void
test_camera_cut_on_loop_wrap(void) {
    assert_camera_cut_on_wrap(false, false, true);
}

static void
test_position_constraint_camera_cut_on_loop_wrap(void) {
    assert_camera_cut_on_wrap(true, false, false);
}

static void
test_aim_constraint_camera_cut_on_loop_wrap(void) {
    assert_camera_cut_on_wrap(false, true, false);
}

static void
test_timeline_seek_clears_fraction(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    uint8_t* entry = fixture(&writer, 1, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    const asset_view_t view = {.data = entry, .size = (uint32_t)(part_row(entry, SCENE_ASSET_HINGES) - entry)};

    enum { TABLE = SCENE_ASSET_AT_OFFSETS + SCENE_ASSET_SETTINGS * sizeof(uint32_t) };

    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(view, &asset));
    const uint32_t overlap = (uint32_t)(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS) - entry);
    test_pack_put32(entry + TABLE, overlap);
    TEST_ASSERT_NOT_EQUAL(ASSET_OK, scene_asset_open(view, &asset));
}

static void
test_timeline_refuses_duplicate_tracks(void) {
    const test_track_t duplicate = {
        CAMERA_NAME, POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, CAMERA_VALUES, KEYS, ANIM_LINEAR};
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
        fixture(&writer, timeline, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    uint8_t* entry = fixture(&writer, 0, 0, 0, 1, false, CAMERA_NAME, POSITION, DURATION);
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
    enum { WING = ANCHOR + 1, FLAP_TRACKS = 3, FLAP_VALUE_BYTES = KEYS * sizeof(float), FLAP_SAMPLE = DURATION / 2 };

    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 0, 2, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_hinge_t wings[] = {
        {.entity = WING, .axis = {0, 0, 1}, .min = -0.5F, .max = 0.5F},
        {.entity = ANCHOR, .axis = {0, 0, 1}, .min = -0.5F, .max = 0.5F},
    };
    memcpy(part_row(entry, SCENE_ASSET_HINGES), wings, sizeof wings);
    for (int row = ANCHOR; row <= WING; row++) {
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + row * sizeof(uint16_t), SUBJECT);
    }
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_tracks_header(clip, FLAP_TRACKS, DURATION);
    const test_track_t tracks[] = {
        {"red", POSITION, ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SUBJECT_VALUES, KEYS, ANIM_LINEAR},
        {"marker", "angle", ANIM_COMPONENT_HINGE, ANIM_VALUE_FLOAT, TIMES, WEIGHT_VALUES, KEYS, ANIM_LINEAR},
        {ANCHOR_NAME, "angle", ANIM_COMPONENT_HINGE, ANIM_VALUE_FLOAT, TIMES, WEIGHT_VALUES + FLAP_VALUE_BYTES, KEYS,
         ANIM_LINEAR},
    };
    for (int i = 0; i < FLAP_TRACKS; i++) {
        test_track_row(clip, i, &tracks[i]);
    }
    const float angles[] = {0, 2.0F, 0, -2.0F};
    test_pack_put_floats(clip + WEIGHT_VALUES, angles, sizeof angles / sizeof *angles);
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
            uint8_t* entry = fixture(&writer, !property, 0, 0, 1, property, CAMERA_NAME, POSITION, DURATION);
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

static uint8_t*
enabled_fixture(test_pack_t* pack, bool hinge, bool constraint, bool lights_only) {
    const size_t capacity = 2 * CAPACITY;
    uint8_t* bytes = test_alloc_aligned(capacity, ASSET_PACK_BASE_ALIGN, &allocation);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(bytes);
    const uint16_t counts[SCENE_ASSET_PART_COUNT] = {[SCENE_ASSET_TIMELINE] = 1,
                                                     [SCENE_ASSET_LIGHTS] = 1,
                                                     [SCENE_ASSET_HINGES] = hinge,
                                                     [SCENE_ASSET_POSITION_CONSTRAINTS] = constraint,
                                                     [SCENE_ASSET_CONSTRAINT_SOURCES] = constraint};
    const size_t sizes[SCENE_ASSET_PART_COUNT] = {
        [SCENE_ASSET_TIMELINE] = sizeof(scene_asset_timeline_t),
        [SCENE_ASSET_HINGES] = sizeof(scene_asset_hinge_t),
        [SCENE_ASSET_POSITION_CONSTRAINTS] = sizeof(scene_asset_position_constraint_t),
        [SCENE_ASSET_CONSTRAINT_SOURCES] = sizeof(scene_asset_constraint_source_t)};
    uint8_t* entry = scene_test_timeline_entry(pack, bytes, capacity, counts, sizes, lights_only);
    test_pack_put16(part_row(entry, SCENE_ASSET_LIGHTS) + offsetof(scene_asset_light_t, entity), VIS_LAMP);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + VIS_CHILD * sizeof(uint16_t), VIS_PARENT);
    const scene_asset_timeline_t timeline = {.clip = "timeline.clip", .wrap = ANIM_CLAMP};
    memcpy(part_row(entry, SCENE_ASSET_TIMELINE), &timeline, sizeof timeline);
    uint8_t* clip = test_pack_add(pack, CLIP, ANIM_TRACKS_ASSET, CLIP_BYTES);

    enum { LIGHT_KEYS = 4, TRACK_STRIDE = 2 * LIGHT_KEYS * sizeof(float), VIS_TRACKS = VIS_ROWS - 1 };

    test_tracks_header(clip, VIS_TRACKS, DURATION);
    const char* names[] = {"red", "green", "blue", LAMP_NAME, "parent", "child"};
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
        test_track_row(clip, i,
                       &(test_track_t){names[i], "enabled", ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, times, values,
                                       keys, ANIM_STEP});
        test_pack_put_floats(clip + times, seconds, keys);
        test_pack_put_floats(clip + values, enabled, keys);
    }
    return entry;
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
    enabled_fixture(&writer, false, false, false);
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
    scene_t* fresh = columns(initial);
    assert_enabled_frame(fresh, b, 0);
    const uint32_t between_edges = (VIS_FIRST + VIS_DARK) / 2;
    timeline_play(initial);
    scene_animation_step(fresh, between_edges);
    for (int i = 0; i < 2; i++) {
        timeline_seek(sought, between_edges);
        assert_enabled_frame(fresh, b, between_edges);
    }
}

static void
test_property_animator_refuses_timeline_only_fields(void) {
    for (int hinge = 0; hinge <= 1; hinge++) {
        for (int property = 0; property <= 1; property++) {
            test_pack_t writer;
            uint8_t* entry = fixture(&writer, !property, 0, 0, hinge, property, CAMERA_NAME, POSITION, DURATION);
            if (property) {
                memcpy(part_row(entry, SCENE_ASSET_CLIPS), CLIP, sizeof CLIP);
            }
            uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
            test_tracks_header(clip, 1, DURATION);
            const test_track_t track = {property ? ANIM_TARGET_SELF : CAMERA_NAME,
                                        hinge ? "angle" : "enabled",
                                        hinge ? ANIM_COMPONENT_HINGE : ANIM_COMPONENT_ENTITY,
                                        ANIM_VALUE_FLOAT,
                                        TIMES,
                                        FOV_VALUES,
                                        KEYS,
                                        hinge ? ANIM_LINEAR : ANIM_STEP};
            test_track_row(clip, 0, &track);
            asset_pack_t pack;
            open_pack(&writer, &pack);
            scene_failure_t failure;
            TEST_ASSERT_EQUAL_INT(property, scene_failure(load(&pack), &failure));
            if (property) {
                TEST_ASSERT_EQUAL_STRING(hinge ? ANIM_TARGET_SELF ":HNGE.angle" : ANIM_TARGET_SELF ":ENTY.enabled",
                                         failure.what);
            }
            release_fixture();
        }
    }
}

static void
test_constraint_refuses_a_hinged_ancestor(void) {
    for (int hinge = 0; hinge <= 1; hinge++) {
        for (int owner = 0; owner <= 1; owner++) {
            test_pack_t writer;
            uint8_t* entry = fixture(&writer, 0, 1, 0, hinge, false, CAMERA_NAME, POSITION, DURATION);
            test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), CAMERA);
            const scene_asset_position_constraint_t constraint = {
                .entity = owner ? ANCHOR : SUBJECT, .source_count = 1, .weight = 1.0F};
            const scene_asset_constraint_source_t source = {name_hash(owner ? "red" : ANCHOR_NAME), 1.0F};
            memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &constraint, sizeof constraint);
            memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
            asset_pack_t pack;
            open_pack(&writer, &pack);
            scene_failure_t failure;
            TEST_ASSERT_EQUAL_INT(hinge, scene_failure(load(&pack), &failure));
            if (hinge) {
                TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
                TEST_ASSERT_EQUAL_STRING("camera:HNGE.angle", failure.what);
            }
            release_fixture();
        }
    }
}

static void
test_enabled_refuses_nonstep_keys(void) {
    test_pack_t writer;
    enabled_fixture(&writer, false, false, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_FALSE(scene_failure(load(&pack), &failure));
    release_fixture();
    enabled_fixture(&writer, false, false, false);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t linear = {
        "green", "enabled",  ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, TIMES, TIMES + 4 * sizeof(float),
        KEYS,    ANIM_LINEAR};
    test_track_row(clip, VIS_GREEN, &linear);
    open_pack(&writer, &pack);
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
    TEST_ASSERT_EQUAL_STRING("green:ENTY.enabled", failure.what);
}

static void
test_enabled_refuses_duplicate_tracks(void) {
    test_pack_t writer;
    enabled_fixture(&writer, false, false, false);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t duplicate = {
        "red", "enabled", ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, TIMES, TIMES + 4 * sizeof(float), KEYS, ANIM_STEP};
    test_track_row(clip, VIS_GREEN, &duplicate);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_STRING("red:ENTY.enabled", failure.what);
}

static void
test_enabled_reapplies_start_on_loop_wrap(void) {
    test_pack_t writer;
    uint8_t* entry = enabled_fixture(&writer, false, false, false);
    part_row(entry, SCENE_ASSET_TIMELINE)[offsetof(scene_asset_timeline_t, wrap)] = ANIM_LOOP;
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const uint32_t values = TIMES + 4 * sizeof(float);
    const float enabled[] = {1.0F, 0.0F};
    test_pack_put_floats(clip + values, enabled, KEYS);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    timeline_play(played);
    scene_animation_step(a, DURATION - 1);
    TEST_ASSERT_FALSE(a->flags[VIS_RED] & SCENE_FLAG_ACTIVE);
    scene_animation_step(a, 1);
    timeline_seek(sought, 0);
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(played));
    const uint8_t state = SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE;
    for (int e = 0; e < VIS_ROWS; e++) {
        TEST_ASSERT_EQUAL_UINT8(b->flags[e] & state, a->flags[e] & state);
    }
    const scene_skinned_renderer_t* red = &a->skinned_renderers[VIS_RED];
    const scene_skinned_renderer_t* fresh = &b->skinned_renderers[VIS_RED];
    TEST_ASSERT_NOT_NULL(red->palette);
    TEST_ASSERT_NOT_NULL(fresh->palette);
    TEST_ASSERT_EQUAL_MEMORY(fresh->palette, red->palette, red->skin.joint_count * sizeof *red->palette);
    for (int e = VIS_GREEN; e < VIS_COPIES; e++) {
        TEST_ASSERT_NULL(a->skinned_renderers[e].palette);
        TEST_ASSERT_NULL(b->skinned_renderers[e].palette);
    }
}

static void
test_enabled_holds_before_first_key(void) {
    test_pack_t writer;
    enabled_fixture(&writer, false, false, false);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t delayed = {
        "red", "enabled", ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, TIMES, TIMES + 4 * sizeof(float), 1, ANIM_STEP};
    test_track_row(clip, VIS_RED, &delayed);
    const float first = (float)VIS_FIRST / MATH_MS_PER_S;
    test_pack_put_floats(clip + TIMES, &first, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    timeline_play(played);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    scene_animation_step(a, VIS_FIRST / 2);
    timeline_seek(sought, VIS_FIRST);
    TEST_ASSERT_FALSE(b->flags[VIS_RED] & SCENE_FLAG_ENABLED);
    for (int i = 0; i < 2; i++) {
        timeline_seek(sought, VIS_FIRST / 2);
        const uint8_t state = SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE;
        TEST_ASSERT_EQUAL_UINT8(state, a->flags[VIS_RED] & state);
        TEST_ASSERT_EQUAL_UINT8(state, b->flags[VIS_RED] & state);
        const scene_skinned_renderer_t* lhs = &a->skinned_renderers[VIS_RED];
        const scene_skinned_renderer_t* rhs = &b->skinned_renderers[VIS_RED];
        TEST_ASSERT_NOT_NULL(lhs->palette);
        TEST_ASSERT_NOT_NULL(rhs->palette);
        TEST_ASSERT_EQUAL_MEMORY(lhs->palette, rhs->palette, lhs->skin.joint_count * sizeof *lhs->palette);
    }
}

static void
take_skin_pressure(void) {
    for (size_t bytes = memory_largest_block(MEMORY_PSRAM); bytes >= sizeof(void*);
         bytes = memory_largest_block(MEMORY_PSRAM)) {
        void** block = memory_alloc(memory_unrounded_bytes(bytes), MEMORY_PSRAM);
        if (block == NULL) {
            break;
        }
        *block = skin_pressure;
        skin_pressure = block;
    }
}

static void
test_enabled_skins_settle_in_row_order_under_pressure(void) {
    enum { EARLY = 100, LATE = 200, SAMPLE = 300 };

    test_pack_t writer;
    enabled_fixture(&writer, false, false, false);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const uint32_t stride = 8 * sizeof(float);
    const float early = (float)EARLY / MATH_MS_PER_S, late = (float)LATE / MATH_MS_PER_S;
    test_pack_put_floats(clip + TIMES + sizeof(float), &late, 1);
    test_pack_put_floats(clip + TIMES + stride + sizeof(float), &early, 1);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    const size_t available = memory_free_bytes(MEMORY_INTERNAL);
    if (available > MEMORY_FAST_INTERNAL_RESERVE) {
        skin_internal_guard = memory_alloc(available - MEMORY_FAST_INTERNAL_RESERVE, MEMORY_INTERNAL);
        TEST_ASSERT_NOT_NULL(skin_internal_guard);
    }
    timeline_seek(played, EARLY);
    TEST_ASSERT_NOT_NULL(a->skinned_renderers[VIS_GREEN].palette);
    take_skin_pressure();
    timeline_seek(played, 0);

    timeline_play(played);
    scene_animation_step(a, EARLY);
    TEST_ASSERT_NOT_NULL(a->skinned_renderers[VIS_GREEN].palette);
    scene_animation_step(a, SAMPLE - EARLY);
    const scene_skinned_renderer_t* winner = &a->skinned_renderers[VIS_RED];
    TEST_ASSERT_NOT_NULL(winner->palette);
    TEST_ASSERT_NULL(a->skinned_renderers[VIS_GREEN].palette);
    scene_failure_t why;
    TEST_ASSERT_TRUE(scene_failure(played, &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_MEMORY, why.status);
    TEST_ASSERT_EQUAL_STRING("green", why.what);
    scene_animation_step(a, 0);
    TEST_ASSERT_NULL(a->skinned_renderers[VIS_GREEN].palette);
    const size_t palette_bytes = winner->skin.joint_count * sizeof *winner->palette;
    const uint32_t played_palette = name_hash_n(winner->palette, palette_bytes);
    timeline_seek(played, 0);
    TEST_ASSERT_FALSE(scene_failure(played, &why));
    timeline_seek(sought, SAMPLE);
    TEST_ASSERT_NOT_NULL(b->skinned_renderers[VIS_RED].palette);
    TEST_ASSERT_NULL(b->skinned_renderers[VIS_GREEN].palette);
    TEST_ASSERT_EQUAL_UINT32(played_palette, name_hash_n(b->skinned_renderers[VIS_RED].palette, palette_bytes));
}

static void
test_enabled_skins_repack_fragmented_storage(void) {
    test_pack_t writer;
    uint8_t* entry = enabled_fixture(&writer, false, false, false);
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_LIGHTS * sizeof(uint16_t), 0);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);

    enum { LIGHT_KEYS = 4, STRIDE = 2 * LIGHT_KEYS * sizeof(float) };

    const float temporary[] = {0.0F, 1.0F, 0.0F};
    const float times[] = {0.0F, (float)VIS_FIRST / MATH_MS_PER_S, (float)VIS_THIRD / MATH_MS_PER_S};
    const test_track_t red = {"red",
                              "enabled",
                              ANIM_COMPONENT_ENTITY,
                              ANIM_VALUE_FLOAT,
                              TIMES,
                              TIMES + LIGHT_KEYS * sizeof(float),
                              sizeof times / sizeof *times,
                              ANIM_STEP};
    test_track_row(clip, VIS_RED, &red);
    test_pack_put_floats(clip + TIMES, times, red.keys);
    test_pack_put_floats(clip + TIMES + STRIDE / 2, temporary, red.keys);

    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    TEST_ASSERT_NULL(a->light_transfer);
    const size_t available = memory_free_bytes(MEMORY_INTERNAL);
    if (available > MEMORY_FAST_INTERNAL_RESERVE) {
        skin_internal_guard = memory_alloc(available - MEMORY_FAST_INTERNAL_RESERVE, MEMORY_INTERNAL);
        TEST_ASSERT_NOT_NULL(skin_internal_guard);
    }
    timeline_seek(played, VIS_FIRST);
    const scene_skinned_renderer_t* small = &a->skinned_renderers[VIS_RED];
    TEST_ASSERT_NOT_NULL(small->storage);
    const size_t unit = (uint8_t*)(small->palette + small->skin.joint_count) - (uint8_t*)small->storage;
    timeline_seek(played, 0);

    scene_skinned_renderer_t* big[] = {&a->skinned_renderers[VIS_BLUE], &b->skinned_renderers[VIS_BLUE]};
    for (size_t i = 0; i < sizeof big / sizeof *big; i++) {
        /* Pad the output reservation while leaving all cluster vertex ranges intact. */
        big[i]->bind.vertex_count += unit / sizeof *big[i]->bind.positions;
        big[i]->skin.vertex_count = big[i]->bind.vertex_count;
    }
    void* space = memory_alloc(3 * unit + unit / 2, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(space);
    take_skin_pressure();
    memory_free(space);

    timeline_play(played);
    scene_animation_step(a, VIS_FIRST);
    TEST_ASSERT_NOT_NULL(a->skinned_renderers[VIS_RED].storage);
    scene_animation_step(a, VIS_SECOND - VIS_FIRST);
    TEST_ASSERT_NOT_NULL(a->skinned_renderers[VIS_GREEN].storage);
    scene_animation_step(a, VIS_THIRD - VIS_SECOND);
    const bool played_big = big[0]->storage != NULL;
    const scene_skinned_renderer_t* green = &a->skinned_renderers[VIS_GREEN];
    TEST_ASSERT_NOT_NULL(green->palette);
    const size_t palette_bytes = green->skin.joint_count * sizeof *green->palette;
    const uint32_t green_palette = name_hash_n(green->palette, palette_bytes);
    const uint32_t blue_palette = played_big ? name_hash_n(big[0]->palette, palette_bytes) : 0;
    timeline_seek(played, 0);

    timeline_seek(sought, VIS_THIRD);
    TEST_ASSERT_NULL(b->skinned_renderers[VIS_RED].storage);
    TEST_ASSERT_NOT_NULL(b->skinned_renderers[VIS_GREEN].storage);
    TEST_ASSERT_NOT_NULL(big[1]->storage);
    TEST_ASSERT_TRUE(played_big);
    TEST_ASSERT_EQUAL_UINT32(green_palette, name_hash_n(b->skinned_renderers[VIS_GREEN].palette, palette_bytes));
    TEST_ASSERT_EQUAL_UINT32(blue_palette, name_hash_n(big[1]->palette, palette_bytes));
}

static void
hinged_light_fixture(test_pack_t* writer, bool delayed, bool scale, bool lights_only) {
    uint8_t* entry = enabled_fixture(writer, true, false, lights_only);
    const scene_asset_hinge_t hinge = {.entity = VIS_LAMP, .axis = {0, 0, 1}, .min = -MATH_PI, .max = MATH_PI};
    memcpy(part_row(entry, SCENE_ASSET_HINGES), &hinge, sizeof hinge);
    uint8_t* parents = part_row(entry, SCENE_ASSET_PARENTS);
    test_pack_put16(parents + VIS_CHILD * sizeof(uint16_t), VIS_LAMP);
    uint8_t* clip = writer->bytes + writer->end - test_pack_aligned(CLIP_BYTES);
    test_tracks_header(clip, scale ? 2 : 1, DURATION);
    const test_track_t angle = {LAMP_NAME,  "angle", ANIM_COMPONENT_HINGE, ANIM_VALUE_FLOAT, TIMES,
                                FOV_VALUES, KEYS,    ANIM_LINEAR};
    test_track_row(clip, 0, &angle);
    const float times[] = {delayed ? (float)VIS_FIRST / MATH_MS_PER_S : 0.0F, 1.0F};
    const float angles[] = {0.0F, MATH_PI / 2};
    test_pack_put_floats(clip + TIMES, times, KEYS);
    test_pack_put_floats(clip + FOV_VALUES, angles, KEYS);
    if (scale) {
        const test_track_t scaling = {
            LAMP_NAME, "scale", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SCALE_VALUES, KEYS, ANIM_LINEAR};
        const float scales[] = {1, 1, 1, 2, 2, 2};
        test_track_row(clip, 1, &scaling);
        test_pack_put_floats(clip + SCALE_VALUES, scales, sizeof scales / sizeof *scales);
    }
}

static void
test_hinge_seek_rebuilds_ancestor_pose(void) {
    test_pack_t writer;
    hinged_light_fixture(&writer, false, false, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    transformf_t child = scene->locals[VIS_CHILD];
    transformf_set_position(&child, vec3f_unit_x());
    scene_row_set_local(scene->root, VIS_CHILD, child);
    const vec3f_t expected = scene_row_world(scene->root, VIS_CHILD).position;
    timeline_seek(root, VIS_SECOND);
    const vec3f_t turned = scene_row_world(scene->root, VIS_CHILD).position;
    TEST_ASSERT_FALSE(vec3f_equal(expected, turned));
    timeline_seek(root, 0);
    const transformf_t actual = scene_row_world(scene->root, VIS_CHILD);
    TEST_ASSERT_EQUAL_FLOAT(expected.x, actual.position.x);
    TEST_ASSERT_EQUAL_FLOAT(expected.y, actual.position.y);
}

static void
test_hinge_moves_root_light_during_play(void) {
    test_pack_t writer;
    hinged_light_fixture(&writer, false, false, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    for (int e = 0; e < VIS_COPIES; e++) {
        scene_row_set_enabled(a->root, e, false);
        scene_row_set_enabled(b->root, e, false);
    }
    scene_animation_step(a, 0);
    timeline_seek(sought, 0);
    scene_resolve(a);
    scene_resolve(b);
    timeline_play(played);

    enum { STEP_MS = 1 };

    for (uint32_t now = STEP_MS; now <= DURATION; now += STEP_MS) {
        scene_animation_step(a, STEP_MS);
        timeline_seek(sought, now);
        const vec3f_t expected = quatf_rotate(b->locals[VIS_LAMP].rotation, SCENE_LIGHT_AXIS);
        TEST_ASSERT_EQUAL_FLOAT(expected.x, b->world_lights.direction[0].x);
        TEST_ASSERT_EQUAL_FLOAT(expected.y, b->world_lights.direction[0].y);
        TEST_ASSERT_EQUAL_FLOAT(expected.z, b->world_lights.direction[0].z);
        TEST_ASSERT_EQUAL_MEMORY(a->world_lights.direction, b->world_lights.direction,
                                 sizeof a->world_lights.direction);
        scene_resolve(a);
        scene_resolve(b);
    }
}

static void
assert_hinge_samples(bool scale) {
    test_pack_t writer;
    hinged_light_fixture(&writer, true, scale, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    timeline_play(played);
    const vec3f_t rest = a->hinges[0].joint.rest.position;
    const uint32_t samples[] = {VIS_FIRST / 2, VIS_SECOND, VIS_THIRD};
    uint32_t previous = 0;
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        const uint32_t now = samples[i];
        scene_animation_step(a, now - previous);
        timeline_seek(sought, now);
        const float t = now < VIS_FIRST ? 0.0F : (float)(now - VIS_FIRST) / (DURATION - VIS_FIRST);
        const quatf_t expected = quatf_from_axis_angle(vec3f_unit_z(), t * (MATH_PI / 2));
        const transformf_t* local = &b->locals[VIS_LAMP];
        TEST_ASSERT_EQUAL_FLOAT(scale ? 1.0F + t : 1.0F, local->scale.x);
        TEST_ASSERT_EQUAL_FLOAT(expected.z, local->rotation.z);
        TEST_ASSERT_EQUAL_FLOAT(expected.w, local->rotation.w);
        TEST_ASSERT_EQUAL_MEMORY(&rest, &local->position, sizeof rest);
        TEST_ASSERT_EQUAL_MEMORY(&a->locals[VIS_LAMP].rotation, &local->rotation, sizeof local->rotation);
        TEST_ASSERT_EQUAL_MEMORY(&a->locals[VIS_LAMP].scale, &local->scale, sizeof local->scale);
        previous = now;
    }
}

static void
test_hinge_samples_inside_limits(void) {
    assert_hinge_samples(false);
}

static void
test_hinge_preserves_timeline_scale(void) {
    assert_hinge_samples(true);
}

static void
test_hinge_reapplies_after_bounds_while_paused(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 0, 1, true, CAMERA_NAME, POSITION, DURATION);
    scene_asset_hinge_t* joint = (void*)part_row(entry, SCENE_ASSET_HINGES);
    joint->entity = SUBJECT;
    joint->angle = 0.5F;
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t lens = {CAMERA_NAME, "near_z",   ANIM_COMPONENT_CAMERA, ANIM_VALUE_FLOAT, TIMES, NEAR_VALUES,
                               KEYS,        ANIM_LINEAR};
    test_track_row(clip, 0, &lens);
    timeline_keep_first_track(&writer);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const entity_t hinged = scene_row_entity(scene, SUBJECT);
    timeline_play(root);
    scene_animation_step(scene, DURATION / 4);
    timeline_pause(root);
    const transformf_t posed = transform_local(hinged);
    transformf_t written = transformf_identity();
    transformf_set_position(&written, vec3f_unit_x());
    transform_set_local(hinged, written);
    (void)scene_bounds(root);
    TEST_ASSERT_TRUE(animator_seek(scene_row_entity(scene, CAMERA), ANIMATOR_CURRENT, DURATION / 2));
    TEST_ASSERT_EQUAL_FLOAT(written.position.x, scene->locals[SUBJECT].position.x);
    scene_animation_step(scene, SAMPLE_FRAME_MS);
    const transformf_t restored = transform_local(hinged);
    TEST_ASSERT_EQUAL_MEMORY(&posed.position, &restored.position, sizeof posed.position);
    TEST_ASSERT_EQUAL_MEMORY(&posed.rotation, &restored.rotation, sizeof posed.rotation);
}

static void
test_keyed_lights_settle_without_animators(void) {
    test_pack_t writer;
    hinged_light_fixture(&writer, false, false, true);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_tracks_header(clip, 2, DURATION);
    const test_track_t tracks[] = {
        {LAMP_NAME, "angle", ANIM_COMPONENT_HINGE, ANIM_VALUE_FLOAT, TIMES, FOV_VALUES, KEYS, ANIM_STEP},
        {"parent", "enabled", ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, CAMERA_VALUES, NEAR_VALUES, KEYS, ANIM_STEP},
    };
    for (size_t i = 0; i < sizeof tracks / sizeof *tracks; i++) {
        test_track_row(clip, i, &tracks[i]);
    }
    const float angle_times[] = {0.0F, (float)VIS_FIRST / MATH_MS_PER_S};
    const float enable_times[] = {0.0F, (float)VIS_SECOND / MATH_MS_PER_S};
    const float enabled[] = {1.0F, 0.0F};
    test_pack_put_floats(clip + TIMES, angle_times, KEYS);
    test_pack_put_floats(clip + CAMERA_VALUES, enable_times, KEYS);
    test_pack_put_floats(clip + NEAR_VALUES, enabled, KEYS);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t roots[] = {load(&pack), load(&pack)};
    for (size_t i = 0; i < sizeof roots / sizeof *roots; i++) {
        TEST_ASSERT_EQUAL_INT(0, columns(roots[i])->animator_count);
        TEST_ASSERT_TRUE(light_set_intensity(entity_find(roots[i], name_hash("parent")), 1.0F));
    }
    scene_t* a = columns(roots[0]);
    const scene_t* b = columns(roots[1]);
    timeline_play(roots[0]);
    const uint32_t keys[] = {0, VIS_FIRST, VIS_SECOND};
    uint32_t previous = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        scene_animation_step(a, keys[i] - previous);
        timeline_seek(roots[1], keys[i]);
        TEST_ASSERT_EQUAL_INT(keys[i] < VIS_SECOND ? 2 : 1, a->world_lights.count);
        TEST_ASSERT_EQUAL_INT(a->world_lights.count, b->world_lights.count);
        const float angle = keys[i] < VIS_FIRST ? 0.0F : MATH_PI / 2;
        const vec3f_t direction = quatf_rotate(quatf_from_axis_angle(vec3f_unit_z(), angle), SCENE_LIGHT_AXIS);
        TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, direction.x, a->world_lights.direction[0].x);
        TEST_ASSERT_FLOAT_WITHIN(TRANSFORMF_MATRIX_TOLERANCE, direction.y, a->world_lights.direction[0].y);
        TEST_ASSERT_EQUAL_MEMORY(a->world_lights.direction, b->world_lights.direction,
                                 sizeof a->world_lights.direction);
        TEST_ASSERT_EQUAL_MEMORY(a->world_lights.color, b->world_lights.color, sizeof a->world_lights.color);
        previous = keys[i];
    }
}

static void
test_hinge_at_rest_leaves_row_clean(void) {
    test_pack_t writer;
    hinged_light_fixture(&writer, false, false, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    scene_animation_step(scene, 0);
    scene_resolve(scene);
    scene_animation_step(scene, 0);
    TEST_ASSERT_FALSE(scene->flags[VIS_LAMP] & SCENE_FLAG_CHANGED);
    timeline_seek(root, VIS_SECOND);
    TEST_ASSERT_TRUE(scene->flags[VIS_LAMP] & SCENE_FLAG_CHANGED);
}

static void
test_enabled_seek_matches_play_with_constraint_and_hinge(void) {
    test_pack_t writer;
    uint8_t* entry = enabled_fixture(&writer, true, true, false);
    const scene_asset_position_constraint_t constraint = {
        .entity = VIS_PARENT, .source_count = 1, .weight = 1.0F, .translation_offset = {1, 2, 3}};
    const scene_asset_constraint_source_t source = {name_hash(CAMERA_NAME), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
    const scene_asset_hinge_t hinge = {.entity = VIS_CHILD, .axis = {0, 0, 1}, .min = -MATH_PI, .max = MATH_PI};
    memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &constraint, sizeof constraint);
    memcpy(part_row(entry, SCENE_ASSET_HINGES), &hinge, sizeof hinge);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t tracks[] = {
        {"parent", "enabled", ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, TIMES, NEAR_VALUES, KEYS, ANIM_STEP},
        {"child", "angle", ANIM_COMPONENT_HINGE, ANIM_VALUE_FLOAT, TIMES, FOV_VALUES, KEYS, ANIM_LINEAR},
        {"red", "enabled", ANIM_COMPONENT_ENTITY, ANIM_VALUE_FLOAT, TIMES, NEAR_VALUES, KEYS, ANIM_STEP},
    };
    test_tracks_header(clip, sizeof tracks / sizeof *tracks, DURATION);
    for (size_t i = 0; i < sizeof tracks / sizeof *tracks; i++) {
        test_track_row(clip, i, &tracks[i]);
    }
    const float times[] = {0.0F, (float)VIS_SECOND / MATH_MS_PER_S};
    const float enabled[] = {0.0F, 1.0F}, angles[] = {0.0F, MATH_PI / 2};
    test_pack_put_floats(clip + TIMES, times, KEYS);
    test_pack_put_floats(clip + NEAR_VALUES, enabled, KEYS);
    test_pack_put_floats(clip + FOV_VALUES, angles, KEYS);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t played = load(&pack), sought = load(&pack);
    scene_t* a = columns(played);
    scene_t* b = columns(sought);
    timeline_play(played);
    const uint8_t state = SCENE_FLAG_ENABLED | SCENE_FLAG_ACTIVE;
    for (uint32_t now = VIS_FIRST; now <= VIS_THIRD; now += VIS_FIRST) {
        scene_animation_step(a, VIS_FIRST);
        timeline_seek(sought, now);
        for (int e = 0; e < VIS_ROWS; e++) {
            const transformf_t x = scene_row_world(a->root, e), y = scene_row_world(b->root, e);
            TEST_ASSERT_EQUAL_UINT8(a->flags[e] & state, b->flags[e] & state);
            TEST_ASSERT_EQUAL_MEMORY(&x.position, &y.position, sizeof x.position);
            TEST_ASSERT_EQUAL_MEMORY(&x.rotation, &y.rotation, sizeof x.rotation);
        }
        TEST_ASSERT_EQUAL_INT(now >= VIS_SECOND, (a->flags[VIS_CHILD] & SCENE_FLAG_ACTIVE) != 0);
        for (int e = 0; e < VIS_COPIES; e++) {
            const scene_skinned_renderer_t* x = &a->skinned_renderers[e];
            const scene_skinned_renderer_t* y = &b->skinned_renderers[e];
            const bool visible = e != VIS_RED || now >= VIS_SECOND;
            TEST_ASSERT_EQUAL_INT(visible, x->palette != NULL);
            TEST_ASSERT_EQUAL_INT(visible, y->palette != NULL);
            if (visible) {
                TEST_ASSERT_EQUAL_MEMORY(x->palette, y->palette, x->skin.joint_count * sizeof *x->palette);
            }
        }
    }
    const entity_t fresh = load(&pack);
    scene_t* c = columns(fresh);
    scene_animation_step(a, 1);
    timeline_seek(fresh, VIS_THIRD + 1);
    scene_resolve(a);
    scene_resolve(c);
    TEST_ASSERT_EQUAL_UINT8(c->flags[VIS_CHILD], a->flags[VIS_CHILD]);
    const vec3f_t target = scene_row_world(c->root, VIS_CAMERA).position;
    assert_constraint_vector(vec3f_add(target, constraint.translation_offset),
                             scene_row_world(c->root, VIS_CHILD).position);
    TEST_ASSERT_FALSE(c->flags[VIS_CHILD] & SCENE_FLAG_PENDING_PLACEMENT);
}

static void
test_constraint_reacts_without_timeline(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_position_constraint_t row = {.entity = CAMERA, .source_count = 1, .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
    const scene_asset_constraint_source_t source = {name_hash(ANCHOR_NAME), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    const vec3f_t destination = {7, 8, 9};
    const entity_t target = entity_find(root, name_hash(ANCHOR_NAME));
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
        uint8_t* entry = fixture(&writer, timed, 1, 0, 0, false, "marker", POSITION, DURATION);
        const scene_asset_position_constraint_t row = {.entity = CAMERA, .source_count = 1, .weight = 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
        const scene_asset_constraint_source_t source = {name_hash(ANCHOR_NAME), 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
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
        transformf_t child = scene->locals[ANCHOR];
        child.position = vec3f_unit_x();
        scene_row_set_local(scene->root, ANCHOR, child);
        scene_row_set_local(scene->root, SUBJECT, parent);
        scene_animation_step(scene, STEP_MAX);
        TEST_ASSERT_EQUAL_FLOAT(initial + parent.scale.x, scene->locals[CAMERA].position.x);
        scene->flags[CAMERA] &= (uint8_t)~SCENE_FLAG_PLACED;
        scene_animation_step(scene, STEP_MAX);
        TEST_ASSERT_EQUAL_UINT8(0, scene->flags[CAMERA] & SCENE_FLAG_PLACED);
        release_fixture();
    }
}

static void
test_constraint_uses_weighted_world_pose(void) {
    enum { POSITION_ONLY = 1, AIM_ONLY = 2, BOTH = POSITION_ONLY | AIM_ONLY };

    for (int mode = POSITION_ONLY; mode <= BOTH; mode++) {
        const bool position = (mode & POSITION_ONLY) != 0, aim = (mode & AIM_ONLY) != 0;
        const float weight = 0.35F;
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, position, aim, 0, false, CAMERA_NAME, POSITION, DURATION);
        test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
        if (position) {
            scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
            row->entity = ANCHOR;
            row->weight = weight;
        }
        if (aim) {
            scene_asset_aim_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS);
            row->entity = ANCHOR;
            row->weight = weight;
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_t* scene = columns(load(&pack));
        transformf_t frame = transformf_identity();
        transformf_set_position(&frame, (vec3f_t){5, 7, 11});
        transformf_set_rotation(&frame, quatf_from_axis_angle(vec3f_unit_z(), MATH_PI / 2.0F));
        transformf_set_scale(&frame, (vec3f_t){2, 2, 2});
        scene_row_set_local(scene->root, SUBJECT, frame);
        const vec3f_t target = scene->locals[CAMERA].position;
        const vec3f_t eye = position ? vec3f_lerp(frame.position, target, weight) : frame.position;
        const quatf_t rotation =
            aim ? quatf_nlerp(frame.rotation, quatf_looking(vec3f_sub(target, eye), vec3f_up()), weight)
                : frame.rotation;
        scene_animation_step(scene, 0);
        const transformf_t got = scene_row_world(scene->root, ANCHOR);
        const transformf_t* local = &scene->locals[ANCHOR];
        assert_constraint_vector(eye, got.position);
        assert_constraint_vector(transformf_inverse_point(&frame, eye), local->position);
        const float tolerance = TRANSFORMF_MATRIX_TOLERANCE;
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 1.0F, fabsf(quatf_mul(quatf_conjugate(rotation), got.rotation).w));
        const quatf_t local_rotation = quatf_mul(quatf_conjugate(frame.rotation), rotation);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 1.0F, fabsf(quatf_mul(quatf_conjugate(local_rotation), local->rotation).w));
        release_fixture();
    }
}

static void
constraint_source_parent_fixture(test_pack_t* writer, bool timed, bool aim) {
    uint8_t* entry = fixture(writer, timed, !aim, aim, 0, false, ANCHOR_NAME, POSITION, DURATION);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
    const scene_asset_constraint_source_t source = {name_hash(ANCHOR_NAME), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
}

static void
test_aim_constraint_uses_source_ancestors(void) {
    test_pack_t writer;
    constraint_source_parent_fixture(&writer, false, true);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_t* scene = columns(load(&pack));
    transformf_t parent = scene->locals[SUBJECT], child = scene->locals[ANCHOR];
    transformf_set_position(&parent, vec3f_unit_y());
    transformf_set_scale(&parent, (vec3f_t){2, 2, 2});
    transformf_set_position(&child, vec3f_unit_x());
    scene_row_set_local(scene->root, SUBJECT, parent);
    scene_row_set_local(scene->root, ANCHOR, child);
    const vec3f_t target = {2, 1, 0};
    const vec3f_t direction = vec3f_sub(target, scene->locals[CAMERA].position);
    scene_animation_step(scene, 0);
    const vec3f_t actual = quatf_rotate(scene->locals[CAMERA].rotation, vec3f_forward());
    assert_constraint_vector(vec3f_normalize(direction), actual);
}

static void
test_constraint_owner_under_zero_scale_keeps_local(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), SUBJECT);
    const scene_asset_position_constraint_t row = {.entity = ANCHOR, .source_count = 1, .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
    const scene_asset_constraint_source_t source = {name_hash(CAMERA_NAME), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const float scale[] = {1, 1, 1, 0, 0, 0};
    test_pack_put_floats(clip + SCALE_VALUES, scale, sizeof scale / sizeof *scale);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    const vec3f_t authored = scene->positions[0].own;
    timeline_seek(root, DURATION);
    TEST_ASSERT_EQUAL_MEMORY(&authored, &scene->locals[ANCHOR].position, sizeof authored);
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
    uint8_t* entry = fixture(&writer, 1, 1, 0, 0, false, "red", POSITION, DURATION);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    const scene_asset_position_constraint_t row = {.entity = CAMERA, .source_count = 1, .weight = 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
    const scene_asset_constraint_source_t source = {name_hash("red"), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    const test_track_t follow = {"red", POSITION,   ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SUBJECT_VALUES,
                                 KEYS,  ANIM_LINEAR};
    test_tracks_header(clip, 1, DURATION);
    test_track_row(clip, 0, &follow);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    assert_camera_history(&pack, false);
}

static void
test_constraint_refuses_bad_control_keys(void) {
    const float bad[] = {-1.0F, NAN, 1.5F, INFINITY};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        test_pack_t writer;
        weighted_fixture(&writer, ANIM_CLAMP, false);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        test_pack_put_floats(clip + WEIGHT_VALUES + (CONSTRAINT_TRACKS - 1) * KEYS * sizeof(float), &bad[i], 1);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
        TEST_ASSERT_EQUAL_STRING("camera:CPOS.weight", failure.what);
        release_fixture();
    }
}

static void
test_constraint_reports_unknown_row_target(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
    const scene_asset_constraint_source_t source = {name_hash("missing"), 1.0F};
    memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
    TEST_ASSERT_EQUAL_STRING(SCENE_TEST_TIMELINE_ID, failure.what);
}

static void
test_constraint_seek_equals_play(void) {
    enum { SAMPLES = 16 };

    test_pack_t writer;
    weighted_fixture(&writer, ANIM_CLAMP, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_TRUE(scene->skinned_renderers[SUBJECT].bound);
    const uint32_t edges[] = {0, DURATION / 2 - 1, DURATION / 2, DURATION / 2 + 1, DURATION - 1, DURATION};
    for (uint32_t trial = 0; trial < 3; trial++) {
        rng_t rng;
        rng_seed(&rng, RANDOM_SEED + trial);
        for (unsigned i = 0; i < SAMPLES + sizeof edges / sizeof *edges; i++) {
            const uint32_t t = i < SAMPLES ? (uint32_t)rng_below(&rng, DURATION) : edges[i - SAMPLES];
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
            const transformf_t played = scene_row_world(scene->root, CAMERA);
            timeline_pause(root);
            timeline_seek(root, DURATION);
            timeline_seek(root, t);
            const transformf_t sought = scene_row_world(scene->root, CAMERA);
            TEST_ASSERT_EQUAL_MEMORY(&played.position, &sought.position, sizeof played.position);
            TEST_ASSERT_EQUAL_MEMORY(&played.rotation, &sought.rotation, sizeof played.rotation);
            TEST_ASSERT_EQUAL_MEMORY(&played.scale, &sought.scale, sizeof played.scale);
        }
    }
}

static void
test_constraint_seek_equals_play_across_wrap(void) {
    for (int aim = 0; aim <= 1; aim++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, !aim, aim, 0, false, CAMERA_NAME, POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
        const scene_asset_constraint_source_t source = {name_hash("red"), 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        test_tracks_header(clip, 1, DURATION);
        const test_track_t track = {"red", POSITION,   ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, SUBJECT_VALUES,
                                    KEYS,  ANIM_LINEAR};
        test_track_row(clip, 0, &track);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t played = load(&pack), sought = load(&pack);
        scene_t* a = columns(played);
        const scene_t* b = columns(sought);
        timeline_play(played);
        const uint32_t edges[] = {DURATION - 1, DURATION, DURATION + 1};
        uint32_t previous = 0;
        for (unsigned i = 0; i < sizeof edges / sizeof *edges; i++) {
            scene_animation_step(a, edges[i] - previous);
            timeline_seek(sought, edges[i]);
            previous = edges[i];
            TEST_ASSERT_EQUAL_MEMORY(&a->locals[CAMERA].position, &b->locals[CAMERA].position, sizeof(vec3f_t));
            TEST_ASSERT_EQUAL_MEMORY(&a->locals[CAMERA].rotation, &b->locals[CAMERA].rotation, sizeof(quatf_t));
            TEST_ASSERT_EQUAL_UINT8(a->flags[CAMERA], b->flags[CAMERA]);
        }
        release_fixture();
    }
}

static void
test_timeline_samples_constraint_source_ancestors(void) {
    test_pack_t writer;
    constraint_source_parent_fixture(&writer, true, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    const scene_t* scene = columns(root);
    const uint32_t times[] = {DURATION, 0, DURATION / 2};
    for (unsigned i = 0; i < sizeof times / sizeof *times; i++) {
        timeline_seek(root, times[i]);
        const float t = (float)times[i] / DURATION;
        const vec3f_t expected = {1.0F + 3.0F * t + (1.0F + t) * (3.0F + 3.0F * t), 0, -(1.0F + t) * 10.0F};
        assert_constraint_vector(expected, scene_row_world(scene->root, CAMERA).position);
    }
}

static void
test_position_constraint_binding_leaves_rotation(void) {
    const uint8_t modes[] = {SCENE_ASSET_BIND_WORLD_SPACE, SCENE_ASSET_BIND_LOCK_TO_TARGET,
                             SCENE_ASSET_BIND_LOCK_TO_TARGET_WITH_WORLD_UP};
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 0, 0, false, ANCHOR_NAME, POSITION, DURATION);
        const scene_asset_position_constraint_t row = {.entity = CAMERA,
                                                       .source_count = 1,
                                                       .binding_mode = modes[i],
                                                       .weight = 1.0F,
                                                       .translation_offset = {1, 2, 3}};
        const scene_asset_constraint_source_t source = {name_hash(ANCHOR_NAME), 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        const scene_t* scene = columns(root);
        transformf_t target = scene->locals[ANCHOR];
        transformf_set_rotation(&target, quatf_from_axis_angle(vec3f_up(), MATH_PI / 2.0F));
        scene_row_set_local(scene->root, ANCHOR, target);
        timeline_seek(root, DURATION / 2);
        const vec3f_t offset = modes[i] == SCENE_ASSET_BIND_WORLD_SPACE
                                   ? row.translation_offset
                                   : quatf_rotate(target.rotation, row.translation_offset);
        assert_constraint_vector(vec3f_add(scene->locals[ANCHOR].position, offset), scene->locals[CAMERA].position);
        anim_track_t rotation;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_find(&scene->timeline->tracks, name_hash(CAMERA_NAME),
                                                         ANIM_COMPONENT_TRANSFORM, name_hash("rotation"), &rotation));
        float sampled[ANIM_WIDTH_MAX];
        anim_track_sample(
            &rotation, anim_playhead_sample_seconds(&scene->timeline->clock, &scene->timeline->tracks.clip), sampled);
        TEST_ASSERT_EQUAL_MEMORY(sampled, &scene->locals[CAMERA].rotation, sizeof(quatf_t));
        release_fixture();
    }
}

static void
test_constraint_refuses_its_own_output_as_input(void) {
    for (int descendant = 0; descendant <= 1; descendant++) {
        for (int aim = 0; aim <= 1; aim++) {
            test_pack_t writer;
            uint8_t* entry = fixture(&writer, 0, !aim, aim, 0, false, CAMERA_NAME, POSITION, DURATION);
            if (descendant) {
                test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), CAMERA);
            }
            const scene_asset_constraint_source_t source = {name_hash(descendant ? ANCHOR_NAME : CAMERA_NAME), 1.0F};
            memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
            asset_pack_t pack;
            open_pack(&writer, &pack);
            scene_failure_t failure;
            TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
            TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
            TEST_ASSERT_EQUAL_STRING(aim ? "camera:TRNS.rotation" : "camera:TRNS.position", failure.what);
            release_fixture();
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
        POSITION_HINGE,
        AIM_HINGE,
        CASES
    };

    for (int i = 0; i < CASES; i++) {
        const bool property = i <= OWNER_ANCESTOR, hinge = i >= TARGET_HINGE;
        const int positions = i == AIM_HINGE ? 0 : (i == TARGET_CONSTRAINT || i == DUPLICATE ? 2 : 1);
        test_pack_t writer;
        uint8_t* entry =
            fixture(&writer, 0, positions, i == AIM_HINGE, hinge, property, CAMERA_NAME, POSITION, DURATION);
        const bool targets_camera = i == TARGET_PROPERTY || i == TARGET_HINGE;
        if (positions) {
            const scene_asset_position_constraint_t first = {
                .entity = targets_camera || i == OWNER_ANCESTOR ? ANCHOR
                          : i == TARGET_ANCESTOR                ? SUBJECT
                                                                : CAMERA,
                .source_count = 1,
                .weight = 1.0F,
            };
            scene_asset_position_constraint_t* rows = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
            rows[0] = first;
            if (positions > 1) {
                rows[1] = (scene_asset_position_constraint_t){
                    .entity = i == DUPLICATE ? CAMERA : SUBJECT, .source_count = 1, .source_first = 1, .weight = 1.0F};
            }
        }
        if (i == TARGET_ANCESTOR || i == OWNER_ANCESTOR) {
            test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), CAMERA);
        }
        const scene_asset_constraint_source_t sources[] = {
            {name_hash(targets_camera         ? CAMERA_NAME
                       : i == TARGET_ANCESTOR ? ANCHOR_NAME
                                              : "red"),
             1.0F},
            {name_hash("marker"), 1.0F},
        };
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, failure.asset);
        const char* expected[] = {".:TRNS.position",   ".:TRNS.position",   ".:TRNS.position",
                                  ".:TRNS.position",   "red:TRNS.position", SCENE_TEST_TIMELINE_ID,
                                  "camera:HNGE.angle", "camera:HNGE.angle", "camera:HNGE.angle"};
        TEST_ASSERT_EQUAL_STRING(expected[i], failure.what);
        release_fixture();
    }
}

static void
test_clamp_samples_fractional_final_step_key(void) {
    enum { CLAMP_MS = 100 };

    test_pack_t writer;
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, CLAMP_MS);
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
    fixture(&writer, 0, 0, 0, 0, true, CAMERA_NAME, POSITION, CLAMP_MS);
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
        uint8_t* entry = fixture(&writer, 1, 0, 0, 0, true, "red", POSITION, DURATION);
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
    fixture(&writer, 1, 0, 0, 0, true, CAMERA_NAME, POSITION, DURATION);
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
    uint8_t* entry = fixture(&writer, 1, 0, 0, 0, false, "red", POSITION, DURATION);
    part_row(entry, SCENE_ASSET_TIMELINE)[SCENE_ASSET_TIMELINE_AT_WRAP] = ANIM_LOOP;
    timeline_keep_first_track(&writer);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);
    TEST_ASSERT_TRUE(camera_activate(scene_row_entity(scene, CAMERA)));
    render_context_t* render = render_context_main();
    render->kept = true;
    timeline_seek(root, DURATION / 2);
    TEST_ASSERT_TRUE(render->kept);
    timeline_play(root);
    scene_timeline_step(scene, DURATION);
    TEST_ASSERT_TRUE(render->kept);
}

static void
test_timeline_clip_type_root_and_empty_tracks(void) {
    for (int i = 0; i < 3; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    fixture(&writer, 1, 0, 0, 0, true, CAMERA_NAME, POSITION, DURATION);
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
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
        uint8_t* entry = fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
    scene_row_t row;
    TEST_ASSERT_NULL(scene_entity_peek(root, &row));
#if !defined(ESP_PLATFORM) || !CONFIG_LAUNCHER_DEVELOPMENT
    timeline_play(root);
    timeline_seek(root, DURATION);
    timeline_pause(root);
    TEST_ASSERT_FALSE(timeline_playing(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_time_ms(root));
    TEST_ASSERT_EQUAL_UINT32(0, timeline_duration_ms(root));
#endif
}

#if !defined(DEVICE_BUILD)
static void
test_checkpoint_restores_timeline_clock_and_locals(void) {
    test_pack_t writer;
    fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
test_checkpoint_restores_constraint_and_backward_scrub(void) {
    test_pack_t writer;
    weighted_fixture(&writer, ANIM_CLAMP, false);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    const entity_t root = load(&pack);
    scene_t* scene = columns(root);

    timeline_play(root);
    scene_animation_step(scene, DURATION / 4);
    timeline_pause(root);
    const vec3f_t quarter = scene->locals[CAMERA].position;
    checkpoint_t saved = {0};
    scene_checkpoint_capture(&saved, scene);
    timeline_seek(root, DURATION / 2);
    const bool restored = checkpoint_restore(&saved);
    checkpoint_clear(&saved);
    TEST_ASSERT_TRUE(restored);
    TEST_ASSERT_EQUAL_UINT32(DURATION / 4, timeline_time_ms(root));
    TEST_ASSERT_FALSE(timeline_playing(root));
    TEST_ASSERT_EQUAL_MEMORY(&quarter, &scene->locals[CAMERA].position, sizeof quarter);
    timeline_seek(root, DURATION);
    timeline_seek(root, DURATION / 4);
    TEST_ASSERT_EQUAL_MEMORY(&quarter, &scene->locals[CAMERA].position, sizeof quarter);
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
    TEST_ASSERT_EQUAL_UINT16(1, asset.position_constraint_count);
    const scene_asset_position_constraint_t* position = asset.position_constraints;
    TEST_ASSERT_EQUAL_UINT16(CAMERA, position->entity);
    TEST_ASSERT_EQUAL_UINT8(SCENE_ASSET_BIND_LOCK_TO_TARGET_WITH_WORLD_UP, position->binding_mode);
    TEST_ASSERT_EQUAL_UINT8(2, position->source_count);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, position->translation_offset.z);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, position->weight);
    TEST_ASSERT_EQUAL_UINT16(1, asset.aim_constraint_count);
    const scene_asset_aim_constraint_t* aim = asset.aim_constraints;
    TEST_ASSERT_EQUAL_UINT16(CAMERA, aim->entity);
    TEST_ASSERT_EQUAL_UINT8(SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP, aim->world_up_type);
    TEST_ASSERT_EQUAL_UINT16(CAMERA, aim->world_up_object);
    TEST_ASSERT_EQUAL_FLOAT(6.0F, aim->aim_vector.z);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, aim->rotation_offset.w);
    TEST_ASSERT_EQUAL_UINT16(3, asset.source_count);
    TEST_ASSERT_EQUAL_UINT32(name_hash("red"), asset.sources[position->source_first].target);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, asset.sources[position->source_first + 1].weight);
    TEST_ASSERT_EQUAL_UINT32(name_hash("green"), asset.sources[aim->source_first].target);
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
suite_scene_timeline(void) {
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
    RUN_TEST(test_checkpoint_restores_constraint_and_backward_scrub);
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
    RUN_TEST(test_enabled_refuses_duplicate_tracks);
    RUN_TEST(test_enabled_holds_before_first_key);
    RUN_TEST(test_enabled_reapplies_start_on_loop_wrap);
    RUN_TEST(test_enabled_skins_settle_in_row_order_under_pressure);
    RUN_TEST(test_enabled_skins_repack_fragmented_storage);
    RUN_TEST(test_property_animator_refuses_timeline_only_fields);
    RUN_TEST(test_constraint_refuses_a_hinged_ancestor);
    RUN_TEST(test_hinge_seek_rebuilds_ancestor_pose);
    RUN_TEST(test_hinge_moves_root_light_during_play);
    RUN_TEST(test_hinge_samples_inside_limits);
    RUN_TEST(test_hinge_preserves_timeline_scale);
    RUN_TEST(test_hinge_at_rest_leaves_row_clean);
    RUN_TEST(test_hinge_reapplies_after_bounds_while_paused);
    RUN_TEST(test_keyed_lights_settle_without_animators);
    RUN_TEST(test_enabled_seek_matches_play_with_constraint_and_hinge);
    RUN_TEST(test_speed_key_playhead_matches_seek);
    RUN_TEST(test_constraint_reacts_without_timeline);
    RUN_TEST(test_paused_constraint_leaves_owner_clean);
    RUN_TEST(test_constraint_uses_weighted_world_pose);
    RUN_TEST(test_aim_constraint_uses_source_ancestors);
    RUN_TEST(test_constraint_owner_under_zero_scale_keeps_local);
    RUN_TEST(test_constrained_camera_cuts_history);
    RUN_TEST(test_constraint_refuses_bad_control_keys);
    RUN_TEST(test_constraint_reports_unknown_row_target);
    RUN_TEST(test_constraint_seek_equals_play);
    RUN_TEST(test_constraint_seek_equals_play_across_wrap);
    RUN_TEST(test_timeline_samples_constraint_source_ancestors);
    RUN_TEST(test_position_constraint_binding_leaves_rotation);
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
    RUN_TEST(test_constraint_source_curves_match_play_and_seek);
    RUN_TEST(test_clamped_constraint_keeps_end_sample);
    RUN_TEST(test_position_constraint_camera_cut_on_loop_wrap);
    RUN_TEST(test_aim_constraint_camera_cut_on_loop_wrap);
    RUN_TEST(test_pending_position_survives_bounds_and_animator_seek);
    RUN_TEST(test_position_consumes_placement_without_an_animator);
    RUN_TEST(test_position_blend_at_full_weight_returns_exact_target);
    RUN_TEST(test_position_blend_handles_large_opposite_positions);
    RUN_TEST(test_aim_cutoff_uses_relative_direction);
    RUN_TEST(test_aim_rotation_offset_unit_tolerance);
    RUN_TEST(test_aim_parallel_or_coincident_up_stays_finite);
    RUN_TEST(test_aim_only_requires_world_up_vector_in_modes_that_read_it);
    RUN_TEST(test_constraint_weight_zero_restores_the_own_position);
    RUN_TEST(test_constraint_normalises_source_weights);
    RUN_TEST(test_position_uses_ordered_sources_and_clamps_weight);
    RUN_TEST(test_position_weight_key_outside_range_is_refused);
    RUN_TEST(test_aim_weight_key_outside_range_is_refused);
    RUN_TEST(test_runtime_animator_cannot_write_position_weight);
    RUN_TEST(test_runtime_animator_cannot_write_aim_weight);
    RUN_TEST(test_aim_updates_only_for_placed_input_paths);
    RUN_TEST(test_constraint_refuses_a_missing_source_track);
    RUN_TEST(test_aim_coincident_weighted_sources_keep_own_rotation);
    RUN_TEST(test_position_collapsed_parent_keeps_own_local);
    RUN_TEST(test_position_source_app_move_without_timeline);
    RUN_TEST(test_position_source_cannot_own_another_constraint);
    RUN_TEST(test_aim_animated_weight_and_zero_source_sum);
    RUN_TEST(test_aim_weight_track_requires_an_existing_source);
    RUN_TEST(test_parent_frame_maps_position_and_aim_locals);
    RUN_TEST(test_position_constraint_binding_modes);
    RUN_TEST(test_aim_constraint_world_up_and_position_order);
    RUN_TEST(test_constraints_refuse_claimed_writers_and_driven_inputs);
    RUN_TEST(test_constraints_claim_only_their_transform_field);
    RUN_TEST(test_clock_and_immediate_seek);
    RUN_TEST(test_zero_duration_loop_holds_zero);
    RUN_TEST(test_scene_without_a_timeline_ignores_controls);
    RUN_TEST(test_load_names_the_failing_clip_field_or_table);
    RUN_TEST(test_scne_validates_every_new_table);
    scene_unload_all();
    (void)scene_set_take(lender);
}

SUITE_REGISTER(suite_scene_timeline);
