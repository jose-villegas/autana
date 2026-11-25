#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_file.h"
#include "math/scalar/rng.h"
#include "math/scalar/units.h"

#include "anim/anim_tracks_format.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/scene_asset.h"
#include "scene/scene_internal.h"
#include "scene/timeline/timeline.h"
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
    ANCHOR = 2,
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
static const char POSITION[] = "position";
static const char ANCHOR_NAME[] = "anchor";

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
fixture(test_pack_t* pack, int timelines, int positions, int aims, int hinges, bool property, const char* path,
        const char* field, uint32_t duration) {
    uint8_t* bytes = test_alloc_aligned(CAPACITY, ASSET_PACK_BASE_ALIGN, &allocation);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(bytes);
    const uint16_t counts[SCENE_ASSET_PART_COUNT] = {[SCENE_ASSET_TIMELINE] = timelines,
                                                     [SCENE_ASSET_POSITION_CONSTRAINTS] = positions,
                                                     [SCENE_ASSET_AIM_CONSTRAINTS] = aims,
                                                     [SCENE_ASSET_CONSTRAINT_SOURCES] = positions || aims ? 2 : 0,
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
put_constraint_weights(test_pack_t* writer, int count, const char* owner) {
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
            owner, fields[i], ANIM_COMPONENT_POSITION_CONSTRAINT, ANIM_VALUE_FLOAT, TIMES, at, KEYS, ANIM_LINEAR,
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
    put_constraint_weights(writer, blocks ? BLOCK_TRACKS : TRACKS, CAMERA_NAME);
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
    const uint32_t crossing = DURATION / 2;
    const uint32_t samples[] = {0, crossing - 1, crossing, crossing + 1, DURATION - 1, DURATION, DURATION + crossing};
    uint32_t previous = 0;
    timeline_play(played);
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        scene_animation_step(a, samples[i] - previous);
        timeline_seek(sought, samples[i]);
        previous = samples[i];
        const float t = (float)(samples[i] % DURATION) / DURATION;
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
        TEST_ASSERT_EQUAL_UINT8(a->flags[CAMERA], b->flags[CAMERA]);
    }
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
    put_constraint_weights(&writer, TRACKS, ANCHOR_NAME);
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
test_constraint_normalises_a_single_source(void) {
    const float weights[] = {0.0F, 0.5F};
    for (size_t i = 0; i < sizeof weights / sizeof *weights; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        const scene_asset_constraint_source_t source = {name_hash(ANCHOR_NAME), weights[i]};
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[ANCHOR].position = (vec3f_t){4, -2, 1};
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const scene_t* scene = columns(load(&pack));
        const vec3f_t expected = scene->asset.transforms[i == 0 ? CAMERA : ANCHOR].position;
        TEST_ASSERT_EQUAL_MEMORY(&expected, &scene->locals[CAMERA].position, sizeof expected);
        release_fixture();
    }
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
test_position_constraint_binding_modes(void) {
    static const struct {
        uint8_t mode;
        vec3f_t expected;
    } cases[] = {
        {SCENE_ASSET_BIND_WORLD_SPACE, {0, 1, 0}},
        {SCENE_ASSET_BIND_LOCK_TO_TARGET, {-1, 0, 0}},
        {SCENE_ASSET_BIND_LOCK_TO_TARGET_WITH_WORLD_UP, {0, 1, 0}},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 0, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[ANCHOR].linear = (mat3f_t){{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}};
        const scene_asset_position_constraint_t row = {
            .entity = CAMERA,
            .binding_mode = cases[i].mode,
            .source_count = 1,
            .weight = 1.0F,
            .translation_offset = {0, 1, 0},
        };
        const scene_asset_constraint_source_t source = {name_hash(ANCHOR_NAME), 1.0F};
        memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &row, sizeof row);
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), &source, sizeof source);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const scene_t* scene = columns(load(&pack));
        assert_constraint_vector(cases[i].expected, scene->locals[CAMERA].position);
        release_fixture();
    }
}

static void
test_aim_constraint_world_up_and_position_order(void) {
    const uint8_t modes[] = {SCENE_ASSET_WORLD_UP_SCENE_UP, SCENE_ASSET_WORLD_UP_OBJECT_UP,
                             SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP, SCENE_ASSET_WORLD_UP_VECTOR,
                             SCENE_ASSET_WORLD_UP_NONE};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 1, 1, 0, false, ANCHOR_NAME, POSITION, DURATION);
        const uint8_t mode = modes[i];
        const bool object = mode == SCENE_ASSET_WORLD_UP_OBJECT_UP || mode == SCENE_ASSET_WORLD_UP_OBJECT_ROTATION_UP;
        const vec3f_t aim_axis = vec3f_unit_x(), up_axis = vec3f_forward();
        const quatf_t offset = quatf_from_axis_angle(aim_axis, MATH_PI / 4.0F);
        const scene_asset_aim_constraint_t aim = {
            .entity = CAMERA,
            .world_up_type = mode,
            .source_count = 1,
            .source_first = 1,
            .world_up_object = object ? ANCHOR : SCENE_ASSET_WORLD_UP_OBJECT_NONE,
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
        const scene_asset_constraint_source_t sources[] = {{name_hash("red"), 1.0F}, {name_hash("red"), 1.0F}};
        memcpy(part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS), &position, sizeof position);
        memcpy(part_row(entry, SCENE_ASSET_AIM_CONSTRAINTS), &aim, sizeof aim);
        memcpy(part_row(entry, SCENE_ASSET_CONSTRAINT_SOURCES), sources, sizeof sources);
        scene_asset_transform_t* transforms = (void*)part_row(entry, SCENE_ASSET_TRANSFORMS);
        transforms[CAMERA].linear = (mat3f_t){{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};
        const quatf_t own = quatf_from_axis_angle(aim_axis, MATH_PI / 2.0F);
        uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
        const test_track_t turn = {
            ANCHOR_NAME, "rotation",  ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_QUAT, TIMES, ROTATION_VALUES,
            KEYS,        ANIM_LINEAR,
        };
        test_track_row(clip, 4, &turn);
        const float turns[] = {0, 0, 0, 1, own.x, own.y, own.z, own.w};
        test_pack_put_floats(clip + ROTATION_VALUES, turns, sizeof turns / sizeof *turns);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        const entity_t root = load(&pack);
        const scene_t* scene = columns(root);
        const uint32_t samples[] = {0, DURATION};
        for (size_t s = 0; s < sizeof samples / sizeof *samples; s++) {
            timeline_seek(root, samples[s]);
            const transformf_t* at = &scene->locals[CAMERA];
            const transformf_t* target = &scene->locals[SUBJECT];
            const transformf_t* up_object = &scene->locals[ANCHOR];
            assert_constraint_vector(vec3f_add(target->position, position.translation_offset), at->position);
            const vec3f_t direction = vec3f_normalize(vec3f_sub(target->position, at->position));
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
    enum { TIMELINE_WRITER, PROPERTY_WRITER, SOURCE_ANCESTOR, UP_ANCESTOR, HINGED_SOURCE, HINGED_UP };

    for (int test = TIMELINE_WRITER; test <= HINGED_UP; test++) {
        const bool up = test == UP_ANCESTOR || test == HINGED_UP;
        const bool hinge = test >= HINGED_SOURCE;
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, test == TIMELINE_WRITER, !up, up, hinge, test != TIMELINE_WRITER && !hinge,
                                 CAMERA_NAME, POSITION, DURATION);
        const bool writer_conflict = test <= PROPERTY_WRITER;
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
            row->world_up_object = ANCHOR;
        } else if (!writer_conflict) {
            scene_asset_position_constraint_t* row = (void*)part_row(entry, SCENE_ASSET_POSITION_CONSTRAINTS);
            row->entity = SUBJECT;
        }
        if (!writer_conflict) {
            test_pack_put16(part_row(entry, SCENE_ASSET_PARENTS) + ANCHOR * sizeof(uint16_t), CAMERA);
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, failure.status);
        if (!hinge) {
            TEST_ASSERT_EQUAL_STRING(test == TIMELINE_WRITER ? "camera:TRNS.position" : ".:TRNS.position",
                                     failure.what);
        } else {
            TEST_ASSERT_NOT_NULL(strstr(failure.what, CAMERA_NAME));
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
        {CAMERA_NAME, POSITION, "hinge", .hinge = true},
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
        const bool unsupported = cases[i].hinge;
        TEST_ASSERT_EQUAL_INT(unsupported ? SCENE_ERR_UNSUPPORTED : SCENE_ERR_ASSET, failure.status);
        TEST_ASSERT_EQUAL_INT(unsupported        ? ASSET_OK
                              : cases[i].missing ? ASSET_ERR_NOT_FOUND
                                                 : ASSET_ERR_FORMAT,
                              failure.asset);
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

    const struct {
        uint8_t* at;
        float bad;
    } floats[] = {
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_TRANSLATION_OFFSET, NAN},
        {position + SCENE_ASSET_POSITION_CONSTRAINT_AT_WEIGHT, -0.5F},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_WEIGHT, 1.5F},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_AIM_VECTOR + offsetof(vec3f_t, z), 0.0F},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_UP_VECTOR + offsetof(vec3f_t, y), 0.0F},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_WORLD_UP_VECTOR, INFINITY},
        {aim + SCENE_ASSET_AIM_CONSTRAINT_AT_ROTATION_OFFSET, NAN},
        {source + SCENE_ASSET_CONSTRAINT_SOURCE_AT_WEIGHT, 1.5F},
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
test_camera_cut_on_loop_wrap(void) {
    test_pack_t writer;
    uint8_t* entry = fixture(&writer, 1, 0, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
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
test_hinge_unsupported_without_timeline(void) {
    test_pack_t writer;
    fixture(&writer, 0, 0, 0, 1, false, CAMERA_NAME, POSITION, DURATION);
    asset_pack_t pack;
    open_pack(&writer, &pack);
    scene_failure_t failure;
    TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_UNSUPPORTED, failure.status);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, failure.asset);
    TEST_ASSERT_EQUAL_STRING("hinge", failure.what);
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
    uint8_t* clip = writer.bytes + writer.end - test_pack_aligned(CLIP_BYTES);
    test_pack_put16(clip + ANIM_TRACKS_AT_COUNT, 1);
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
    RUN_TEST(test_checkpoint_restores_timeline_clock_and_locals);
    RUN_TEST(test_python_baked_v7_motion_row_layout);
    RUN_TEST(test_keys_glb_blocks_play_through_scene_load);
#endif
    RUN_TEST(test_timeline_refuses_duplicate_tracks);
    RUN_TEST(test_scene_writers_refuse_bad_lens_keys);
    RUN_TEST(test_hinge_unsupported_without_timeline);
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
    RUN_TEST(test_constraint_weight_zero_restores_the_own_position);
    RUN_TEST(test_constraint_normalises_a_single_source);
    RUN_TEST(test_constraint_refuses_a_missing_source_track);
    RUN_TEST(test_position_constraint_binding_modes);
    RUN_TEST(test_aim_constraint_world_up_and_position_order);
    RUN_TEST(test_constraints_refuse_claimed_writers_and_driven_inputs);
    RUN_TEST(test_clock_and_immediate_seek);
    RUN_TEST(test_zero_duration_loop_holds_zero);
    RUN_TEST(test_scene_without_a_timeline_ignores_controls);
    RUN_TEST(test_load_names_the_failing_clip_field_or_table);
    RUN_TEST(test_scne_validates_every_new_table);
    scene_unload_all();
    (void)scene_set_take(lender);
}

SUITE_REGISTER(suite_scene_timeline);
