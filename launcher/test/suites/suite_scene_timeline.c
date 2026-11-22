#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_file.h"
#include "math/scalar/rng.h"

#include "anim/anim_tracks_format.h"
#include "render/context/render_context.h"
#include "scene/animator/animator.h"
#include "scene/scene_asset.h"
#include "scene/scene_internal.h"
#include "scene/timeline/timeline.h"
#include "suites.h"
#include "test_alloc.h"
#include "test_anim_tracks.h"
#include "test_scene_timeline.h"
#include "testkit/cleanup.h"

#include "../scene_examples/timeline.c"

enum {
    CAPACITY = 8192,
    ARENA_BYTES = 4096,
    DURATION = 1000,
    STEP_MAX = 97,
    SAMPLES = 12,
    CAMERA = 1,
    SUBJECT = 0,
    KEYS = 2,
    TRACKS = 6,
    RANDOM_SEED = 0x7351U,
    TIMES = TEST_TRACKS_HEADER_SIZE + TRACKS * (TEST_TRACK_ROW_SIZE + TEST_TRACK_STRING_SLOT),
    CAMERA_VALUES = TIMES + KEYS * sizeof(float),
    SUBJECT_VALUES = CAMERA_VALUES + KEYS * 3 * sizeof(float),
    FOV_VALUES = SUBJECT_VALUES + KEYS * 3 * sizeof(float),
    NEAR_VALUES = FOV_VALUES + KEYS * sizeof(float),
    ROTATION_VALUES = NEAR_VALUES + KEYS * sizeof(float),
    SCALE_VALUES = ROTATION_VALUES + KEYS * 4 * sizeof(float),
    CLIP_BYTES = SCALE_VALUES + KEYS * 3 * sizeof(float)
};

static const char CLIP[] = "timeline.clip";
static const char CAMERA_NAME[] = "camera";
static const char POSITION[] = "position";

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
test_seek_matches_play_at_the_same_millisecond(void) {
    rng_t random;
    rng_seed(&random, RANDOM_SEED);
    const anim_wrap_t wraps[] = {ANIM_CLAMP, ANIM_LOOP};
    for (size_t mode = 0; mode < sizeof wraps / sizeof *wraps; mode++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, 0, 0, false, CAMERA_NAME, POSITION, DURATION);
        part_row(entry, SCENE_ASSET_TIMELINE)[offsetof(scene_asset_timeline_t, wrap)] = wraps[mode];
        asset_pack_t pack;
        open_pack(&writer, &pack);
        for (int i = 0; i < SAMPLES; i++) {
            const uint32_t target = (uint32_t)rng_below(&random, DURATION) + (i % 3) * DURATION;
            const entity_t played = load(&pack), sought = load(&pack);
            scene_t* a = columns(played);
            scene_t* b = columns(sought);
            timeline_play(played);
            for (uint32_t elapsed = 0; elapsed < target;) {
                const uint32_t draw = (uint32_t)rng_below(&random, STEP_MAX) + 1;
                const uint32_t dt = draw < target - elapsed ? draw : target - elapsed;
                a->animators[SUBJECT].drawn_radius_px = INFINITY;
                scene_animation_step(a, dt);
                elapsed += dt;
            }
            scene_skins_settle(b);
            b->animators[SUBJECT].drawn_radius_px = INFINITY;
            TEST_ASSERT_TRUE(animator_seek(scene_row_entity(b, SUBJECT), ANIMATOR_CURRENT, target));
            timeline_seek(sought, target);
            const uint32_t expected =
                wraps[mode] == ANIM_LOOP ? target % DURATION : (target < DURATION ? target : DURATION);
            TEST_ASSERT_EQUAL_UINT32(expected, timeline_time_ms(played));
            TEST_ASSERT_EQUAL_UINT32(timeline_time_ms(played), timeline_time_ms(sought));
            for (int e = 0; e < a->asset.entity_count; e++) {
                TEST_ASSERT_EQUAL_MEMORY(&a->locals[e].position, &b->locals[e].position, sizeof(vec3f_t));
                TEST_ASSERT_EQUAL_MEMORY(&a->locals[e].rotation, &b->locals[e].rotation, sizeof(quatf_t));
                TEST_ASSERT_EQUAL_MEMORY(&a->locals[e].scale, &b->locals[e].scale, sizeof(vec3f_t));
            }
            TEST_ASSERT_EQUAL_MEMORY(&a->cameras[0].lens, &b->cameras[0].lens, sizeof a->cameras[0].lens);
            const scene_skinned_renderer_t* x = &a->skinned_renderers[SUBJECT];
            const scene_skinned_renderer_t* y = &b->skinned_renderers[SUBJECT];
            TEST_ASSERT_NOT_NULL(x->palette);
            TEST_ASSERT_NOT_NULL(y->palette);
            TEST_ASSERT_EQUAL_MEMORY(x->palette, y->palette, x->skin.joint_count * sizeof(*x->palette));
            scene_release(sought);
            scene_release(played);
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
        bool missing, property, constraint, hinge;
    } cases[] = {
        {CAMERA_NAME, POSITION, "missing", .missing = true},
        {.path = "missing", .field = POSITION, .what = CLIP},
        {.path = CAMERA_NAME, .field = "missing", .what = CLIP},
        {.path = ANIM_TARGET_SELF, .field = POSITION, .what = CLIP},
        {CAMERA_NAME, POSITION, "camera.position", .property = true},
        {CAMERA_NAME, POSITION, "constraint", .constraint = true},
        {CAMERA_NAME, POSITION, "hinge", .hinge = true},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_pack_t writer;
        uint8_t* entry = fixture(&writer, 1, cases[i].constraint, cases[i].hinge, cases[i].property, cases[i].path,
                                 cases[i].field, DURATION);
        if (cases[i].missing) {
            memset(part_row(entry, SCENE_ASSET_TIMELINE), 0, ASSET_NAME_MAX);
            memcpy(part_row(entry, SCENE_ASSET_TIMELINE), cases[i].what, strlen(cases[i].what));
        }
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        const bool unsupported = cases[i].constraint || cases[i].hinge;
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
    const size_t target_fields[] = {SCENE_ASSET_CONSTRAINT_AT_FOLLOW, SCENE_ASSET_CONSTRAINT_AT_LOOK_AT};
    for (size_t i = 0; i < sizeof target_fields / sizeof *target_fields; i++) {
        test_pack_put32(constraint + target_fields[i], name_hash("absent"));
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, scene_asset_open(view, &asset));
        test_pack_put32(constraint + target_fields[i], NAME_HASH_NONE);
    }
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
    TEST_ASSERT_EQUAL_STRING("camera.position", failure.what);
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
    TEST_ASSERT_EQUAL_STRING("camera.position", failure.what);
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
        TEST_ASSERT_EQUAL_STRING("camera.near_z", failure.what);
        release_fixture();
    }
}

static void
test_unsupported_rows_without_timeline(void) {
    for (int hinge = 0; hinge <= 1; hinge++) {
        test_pack_t writer;
        fixture(&writer, 0, !hinge, hinge, false, CAMERA_NAME, POSITION, DURATION);
        asset_pack_t pack;
        open_pack(&writer, &pack);
        scene_failure_t failure;
        TEST_ASSERT_TRUE(scene_failure(load(&pack), &failure));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_UNSUPPORTED, failure.status);
        TEST_ASSERT_EQUAL_INT(ASSET_OK, failure.asset);
        TEST_ASSERT_EQUAL_STRING(hinge ? "hinge" : "constraint", failure.what);
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
        TEST_ASSERT_EQUAL_STRING("camera.position", failure.what);
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
#endif
    RUN_TEST(test_timeline_refuses_duplicate_tracks);
    RUN_TEST(test_scene_writers_refuse_bad_lens_keys);
    RUN_TEST(test_unsupported_rows_without_timeline);
    RUN_TEST(test_clamp_samples_fractional_final_step_key);
    RUN_TEST(test_timeline_leaves_resting_transforms_clean);
    RUN_TEST(test_runtime_animator_cannot_take_timeline_field);
    RUN_TEST(test_camera_cut_on_loop_wrap);
    RUN_TEST(test_timeline_seek_clears_fraction);
    RUN_TEST(test_scene_rejects_overlapping_tables);
    RUN_TEST(test_seek_matches_play_at_the_same_millisecond);
    RUN_TEST(test_clock_and_immediate_seek);
    RUN_TEST(test_zero_duration_loop_holds_zero);
    RUN_TEST(test_scene_without_a_timeline_ignores_controls);
    RUN_TEST(test_load_names_the_failing_clip_field_or_table);
    RUN_TEST(test_scne_validates_every_new_table);
}

SUITE_REGISTER(run_scene_timeline_suite);
