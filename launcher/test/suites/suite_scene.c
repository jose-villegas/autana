/*
 * Portable suite: the scene manager (scene/scene.h). Each test loads scenes
 * from a small pack built here: scene entries (SCNE), camera clips (TRCK)
 * and unit quads in distinct colours, so what was drawn is read straight
 * off the picture: a pixel is a quad's colour or the clear colour. The
 * camera stands at z = 10 with a half field of view of 1, which puts a unit
 * of x 3.2 pixels from the centre of a 64-pixel picture.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "anim/anim_tracks.h"
#include "asset/asset_pack.h"
#include "gfx/gfx_color.h"
#include "render/context/render_context.h"
#include "scene/scene.h"
#include "scene/scene_internal.h"
#include "scene/scene_shell.h"
#include "test_alloc.h"
#include "test_anim_tracks.h"
#include "test_cleanup.h"
#include "test_pack.h"
#include "util/runtime/memory.h"

#ifndef DEVICE_BUILD
#include <stdio.h>

#include "asset/asset_file.h"
#include "test_asset_dir.h"
#endif

#define SIZE          64
#define CENTER        (SIZE / 2)
#define CLEAR_RGB     0x336699
#define QUAD_BYTES    132
#define PACK_MAX      8192
#define SENTINEL      0x5A5A

/* Copies of one mesh a test draws, to make a culled list worth measuring. */
#define INSTANCES_MAX 64

/* A unit quad in the plane z = 0, two-sided, in one colour. The arrays sit
 * one after another after the 44-byte header. */
static void
make_quad_entry(uint8_t* entry, uint8_t red, uint8_t green, uint8_t blue) {
    enum {
        POSITIONS = 44,
        COLORS = 68,
        TRIANGLES = 80,
        CLUSTERS = 92,
        CLUSTER_VERTICES = 94,
        CLUSTER_TRIANGLES = 98,
        CLUSTER_LO = 100,
        CLUSTER_HI = 106,
        CLUSTER_DOUBLE_SIDED = 112,
        NODES = 116,
        NODE_COUNT = 130,
        NODE_LEAF = 131,
    };

    const uint32_t words[11] = {4, 2, 1, 1, 1, POSITIONS, COLORS, TRIANGLES, CLUSTERS, NODES, 0};
    for (int i = 0; i < 11; i++) {
        test_pack_put32(entry + (4 * i), words[i]);
    }
    const int xs[4] = {-1, 1, 1, -1};
    const int ys[4] = {-1, -1, 1, 1};
    for (int v = 0; v < 4; v++) {
        test_pack_put16(entry + POSITIONS + (6 * v), xs[v]);
        test_pack_put16(entry + POSITIONS + (6 * v) + 2, ys[v]);
        entry[COLORS + (3 * v)] = red;
        entry[COLORS + (3 * v) + 1] = green;
        entry[COLORS + (3 * v) + 2] = blue;
    }
    const int corners[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        test_pack_put16(entry + TRIANGLES + (2 * i), corners[i]);
    }
    test_pack_put16(entry + CLUSTER_VERTICES, 4);
    test_pack_put16(entry + CLUSTER_TRIANGLES, 2);
    test_pack_put16(entry + CLUSTER_LO, -1);
    test_pack_put16(entry + CLUSTER_LO + 2, -1);
    test_pack_put16(entry + CLUSTER_HI, 1);
    test_pack_put16(entry + CLUSTER_HI + 2, 1);
    entry[CLUSTER_DOUBLE_SIDED] = 1;
    test_pack_put16(entry + NODES, -1);
    test_pack_put16(entry + NODES + 2, -1);
    test_pack_put16(entry + NODES + 6, 1);
    test_pack_put16(entry + NODES + 8, 1);
    entry[NODE_COUNT] = 1;
    entry[NODE_LEAF] = 1;
}

static const struct {
    const char* name;
    uint8_t red, green, blue;
} QUADS[] = {{"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}};

#define QUAD_COUNT 3

/* A clip of the camera node flying 5 units along x in a second, facing down
 * -z. Both tracks share the times; each array starts where the last ends. */
enum { FLIGHT_TIMES = 104, FLIGHT_POSITIONS = 112, FLIGHT_TURNS = 136, FLIGHT_BYTES = 168 };

/* "flight" is the clip as a scene reads it; "skewed" has a translation 2 wide
 * and "unturned" no rotation, which a scene must refuse. */
static const struct {
    const char* id;
    int translation_width, track_count;
} CLIPS[] = {{"flight", 3, 2}, {"skewed", 2, 2}, {"unturned", 3, 1}};

#define CLIP_COUNT ((int)(sizeof CLIPS / sizeof CLIPS[0]))

static void
make_clip_entry(uint8_t* entry, int translation_width, int track_count) {
    const float times[] = {0.0F, 1.0F};
    const float positions[] = {0, 0, 10, 5, 0, 10};
    const float turns[] = {0, 0, 0, 1, 0, 0, 0, 1};
    test_tracks_header(entry, track_count, 1000);
    test_track_row(entry, 0,
                   &(test_track_t){.name = "camera/translation",
                                   .times = FLIGHT_TIMES,
                                   .values = FLIGHT_POSITIONS,
                                   .keys = 2,
                                   .width = translation_width,
                                   .interp = ANIM_LINEAR});
    test_track_row(entry, 1,
                   &(test_track_t){.name = "camera/rotation",
                                   .times = FLIGHT_TIMES,
                                   .values = FLIGHT_TURNS,
                                   .keys = 2,
                                   .width = 4,
                                   .interp = ANIM_LINEAR,
                                   .quaternion = true});
    test_pack_put_floats(entry + FLIGHT_TIMES, times, 2);
    test_pack_put_floats(entry + FLIGHT_POSITIONS, positions, 6);
    test_pack_put_floats(entry + FLIGHT_TURNS, turns, 8);
}

#define IDENTITY {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}}
#define AT(x, y, z)                                                                                                    \
    {                                                                                                                  \
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, { x, y, z }                                                                 \
    }

/* What a scene entry holds, written out as tools/r3d/scene_asset.py would.
 * Every camera has a half field of view and a near plane of 1. */
typedef struct {
    const char* id;
    int entity_count, renderer_count, camera_count;

    struct {
        const char* name;
        scene_transform_t at;
    } entities[3];

    struct {
        int entity;
        const char* mesh;
    } renderers[2];

    struct {
        int entity;
        uint32_t clear_rgb;
        const char* clip;
        const char* node;
    } cameras[2];
} scene_spec_t;

static const scene_spec_t SCENES[] = {
    /* a camera, a red quad at the origin and a green one 4 units right */
    {"test_pair",
     3,
     2,
     1,
     {{"camera", AT(0, 0, 10)}, {"red", IDENTITY}, {"green", AT(4, 0, 0)}},
     {{1, "red"}, {2, "green"}},
     {{0, CLEAR_RGB, "", ""}}},
    /* its own camera and one blue quad at the origin */
    {"test_solo", 2, 1, 1, {{"eye", AT(0, 0, 10)}, {"blue", IDENTITY}}, {{1, "blue"}}, {{0, CLEAR_RGB, "", ""}}},
    /* the same as solo, its camera clearing to a colour of its own */
    {"test_sky", 2, 1, 1, {{"eye", AT(0, 0, 10)}, {"blue", IDENTITY}}, {{1, "blue"}}, {{0, 0x996633, "", ""}}},
    /* names a mesh the pack does not hold */
    {"test_broken", 2, 2, 0, {{"red", IDENTITY}, {"gone", IDENTITY}}, {{0, "red"}, {1, "gone"}}, {{0}}},
    /* a camera that flies the clip "flight", and a red quad */
    {"test_flight",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "flight", "camera"}}},
    /* two cameras, 4 units apart, and a red quad at the origin */
    {"test_twin",
     3,
     1,
     2,
     {{"left", AT(0, 0, 10)}, {"right", AT(4, 0, 10)}, {"red", IDENTITY}},
     {{2, "red"}},
     {{0, CLEAR_RGB, "", ""}, {1, CLEAR_RGB, "", ""}}},
    /* a camera flying a clip the pack does not hold */
    {"test_lost",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "nowhere", "camera"}}},
    /* a camera flying a node the clip does not animate */
    {"test_headless",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "flight", "lamp"}}},
    /* cameras flying a translation of the wrong width, and a clip with no rotation */
    {"test_skewed",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "skewed", "camera"}}},
    {"test_unturned",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "unturned", "camera"}}},
};

#define SCENE_COUNT ((int)(sizeof SCENES / sizeof SCENES[0]))
#define BAD_SCENE   "test_bad" /* the pair, its second renderer naming entity 9 */

static uint32_t
scene_entry_size(const scene_spec_t* s) {
    return 24U + ((uint32_t)s->entity_count * (32U + sizeof(scene_transform_t)))
           + ((uint32_t)s->renderer_count * sizeof(scene_asset_renderer_t))
           + ((uint32_t)s->camera_count * sizeof(scene_asset_camera_t));
}

static void
make_scene_entry(uint8_t* entry, const scene_spec_t* s) {
    const uint32_t names = 24;
    const uint32_t transforms = names + ((uint32_t)s->entity_count * 32U);
    const uint32_t renderers = transforms + ((uint32_t)s->entity_count * sizeof(scene_transform_t));
    const uint32_t cameras = renderers + ((uint32_t)s->renderer_count * sizeof(scene_asset_renderer_t));
    const uint32_t offsets[4] = {names, transforms, renderers, cameras};
    test_pack_put16(entry, SCENE_ASSET_VERSION);
    test_pack_put16(entry + 2, s->entity_count);
    test_pack_put16(entry + 4, s->renderer_count);
    test_pack_put16(entry + 6, s->camera_count);
    for (int i = 0; i < 4; i++) {
        test_pack_put32(entry + 8 + (4 * i), offsets[i]);
    }
    for (int i = 0; i < s->entity_count; i++) {
        memcpy(entry + names + (32U * (uint32_t)i), s->entities[i].name, strlen(s->entities[i].name));
        memcpy(entry + transforms + (sizeof(scene_transform_t) * (size_t)i), &s->entities[i].at,
               sizeof(scene_transform_t));
    }
    for (int i = 0; i < s->renderer_count; i++) {
        uint8_t* row = entry + renderers + (sizeof(scene_asset_renderer_t) * (size_t)i);
        test_pack_put16(row, s->renderers[i].entity);
        memcpy(row + 4, s->renderers[i].mesh, strlen(s->renderers[i].mesh));
    }
    for (int i = 0; i < s->camera_count; i++) {
        uint8_t* row = entry + cameras + (sizeof(scene_asset_camera_t) * (size_t)i);
        const float lens[2] = {1.0F, 1.0F};
        test_pack_put16(row, s->cameras[i].entity);
        test_pack_put_floats(row + 4, lens, 2);
        test_pack_put32(row + 12, s->cameras[i].clear_rgb);
        memcpy(row + 16, s->cameras[i].clip, strlen(s->cameras[i].clip));
        memcpy(row + 48, s->cameras[i].node, strlen(s->cameras[i].node));
    }
}

/* The pack of every quad, scene and the clip, with the CRC set. */
static uint32_t
make_pack(uint8_t* bytes) {
    test_pack_t pack = test_pack_begin(bytes, PACK_MAX, QUAD_COUNT + SCENE_COUNT + 1 + CLIP_COUNT);
    for (int i = 0; i < QUAD_COUNT; i++) {
        uint8_t* entry = test_pack_add(&pack, QUADS[i].name, R3D_LIT_MESH_ASSET, QUAD_BYTES);
        make_quad_entry(entry, QUADS[i].red, QUADS[i].green, QUADS[i].blue);
    }
    for (int i = 0; i < SCENE_COUNT; i++) {
        make_scene_entry(test_pack_add(&pack, SCENES[i].id, SCENE_ASSET, scene_entry_size(&SCENES[i])), &SCENES[i]);
    }
    uint8_t* bad = test_pack_add(&pack, BAD_SCENE, SCENE_ASSET, scene_entry_size(&SCENES[0]));
    make_scene_entry(bad, &SCENES[0]);
    test_pack_put16(bad + 24 + (3 * (32 + sizeof(scene_transform_t))) + sizeof(scene_asset_renderer_t), 9);
    for (int i = 0; i < CLIP_COUNT; i++) {
        make_clip_entry(test_pack_add(&pack, CLIPS[i].id, ANIM_TRACKS_ASSET, FLIGHT_BYTES), CLIPS[i].translation_width,
                        CLIPS[i].track_count);
    }
    return test_pack_finish(&pack);
}

typedef struct {
    void* raw;      /* what the aligned allocation gave */
    uint8_t* bytes; /* aligned inside it, as a pack must be */
    uint16_t* pixels;
    uint16_t* held; /* a picture kept to compare the next with */
    r3d_instance_t* instances;
    asset_pack_t pack;
    scene_target_t target;
} fixture_t;

static fixture_t fx;

static void
release_fixture(void) {
    scene_unload_all();
    test_free_aligned(fx.raw);
    free(fx.pixels);
    free(fx.held);
    free(fx.instances);
    fx = (fixture_t){0};
}

/* Every test starts from an empty engine, so the order of tests is no one's
 * business. */
static void
fixture(void) {
    scene_unload_all();
    fx.bytes = test_alloc_aligned(PACK_MAX, ASSET_PACK_BASE_ALIGN, &fx.raw);
    fx.pixels = malloc(sizeof(*fx.pixels) * SIZE * SIZE);
    fx.held = malloc(sizeof(*fx.held) * SIZE * SIZE);
    fx.instances = malloc(sizeof(*fx.instances) * INSTANCES_MAX);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(fx.bytes);
    TEST_ASSERT_NOT_NULL(fx.pixels);
    TEST_ASSERT_NOT_NULL(fx.held);
    TEST_ASSERT_NOT_NULL(fx.instances);
    const uint32_t total = make_pack(fx.bytes);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&fx.pack, fx.bytes, total));
    fx.target = (scene_target_t){fx.pixels, SIZE, SIZE};
}

static scene_t*
load(const char* name) {
    scene_t* scene = scene_load_from(&fx.pack, name, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, name);
    return scene;
}

/* Loads and activates the camera at full size. */
static scene_t*
show(const char* name, const char* camera) {
    scene_t* scene = load(name);
    TEST_ASSERT_TRUE(scene_activate(scene, camera));
    render_context_set_scale(render_context_main(), 100);
    return scene;
}

/* One frame as the shell composes it, into a picture first filled with a
 * value nothing draws. */
static void
frame(uint32_t dt_ms) {
    for (int i = 0; i < SIZE * SIZE; i++) {
        fx.pixels[i] = SENTINEL;
    }
    scene_compose(dt_ms, 0, &fx.target);
}

static uint16_t
pixel(float units_from_center) {
    return fx.pixels[(CENTER * SIZE) + CENTER + (int)(units_from_center * 3.2F)];
}

static const uint16_t CLEAR = GFX_RGB(CLEAR_RGB);

static void
test_a_scene_loads_by_name_and_unloading_gives_back_everything_it_took(void) {
    fixture();
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    scene_t* scene = load("test_pair");
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) < before);
    scene_unload(scene);
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) == before);
}

static void
test_a_camera_clears_to_the_colour_its_scene_gives(void) {
    fixture();
    scene_t* scene = load("test_sky");
    TEST_ASSERT_TRUE(scene_activate(scene, NULL));
    render_context_set_scale(render_context_main(), 100);
    frame(0);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x996633), pixel(-8.0F));
}

/* Dynamic resolution is opt-in: a camera draws at its own scale until a
 * caller asks, again once it asks no more, and again after the app leaves. */
static void
test_a_camera_keeps_its_fixed_scale_unless_dynamic_resolution_is_asked_for(void) {
    fixture();
    scene_t* scene = show("test_sky", NULL);
    render_context_set_scale(render_context_main(), 50);
    frame(0);
    TEST_ASSERT_EQUAL_INT(-1, render_context_frame(render_context_main()).step);
    TEST_ASSERT_EQUAL_INT(SIZE / 2, render_context_frame(render_context_main()).width);

    const resolution_step_t quarter = {SIZE / 4, SIZE / 4};
    const resolution_config_t one = resolution_config(&quarter, 1, 1, INT32_MAX);
    render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
    frame(0);
    TEST_ASSERT_EQUAL_INT(0, render_context_frame(render_context_main()).step);
    TEST_ASSERT_EQUAL_INT(SIZE / 4, render_context_frame(render_context_main()).width);

    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    frame(0);
    TEST_ASSERT_EQUAL_INT(-1, render_context_frame(render_context_main()).step);
    TEST_ASSERT_EQUAL_INT(SIZE / 2, render_context_frame(render_context_main()).width);

    render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
    (void)scene;
    scene_unload_all();
    scene = show("test_sky", NULL);
    frame(0);
    TEST_ASSERT_EQUAL_INT(-1, render_context_frame(render_context_main()).step);
    TEST_ASSERT_EQUAL_INT(SIZE, render_context_frame(render_context_main()).width);
}

static void
test_a_load_that_fails_says_what_it_was_about(void) {
    fixture();
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_nothing_of_the_kind", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_UNKNOWN, why.status);
    TEST_ASSERT_EQUAL_STRING("test_nothing_of_the_kind", why.what);

    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_broken", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, why.asset);
    TEST_ASSERT_EQUAL_STRING("gone", why.what);
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) == before);

    TEST_ASSERT_NULL(scene_load_from(NULL, "test_pair", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, why.asset);
    TEST_ASSERT_EQUAL_STRING("test_pair", why.what);
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_nothing_of_the_kind", NULL)); /* no one to tell */
}

/* A caller's id too long for a pack name fails as unknown, and the failure
 * holds as much of it as a pack name does. */
static void
test_an_id_too_long_for_a_pack_name_is_cut_where_the_failure_names_it(void) {
    fixture();
    const char* const longest = "test_abcdefghijklmnopqrstuvwxyz";   /* 31 bytes */
    const char* const too_long = "test_abcdefghijklmnopqrstuvwxyz0"; /* 32 bytes */
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, longest, &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_UNKNOWN, why.status);
    TEST_ASSERT_EQUAL_STRING(longest, why.what);
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, too_long, &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_UNKNOWN, why.status);
    TEST_ASSERT_EQUAL_STRING(longest, why.what);
}

/* Each failure names the entry it was about, and the load takes nothing. */
static void
expect_failure(const char* id, asset_status_t asset, const char* what) {
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    scene_failure_t why;
    TEST_ASSERT_NULL_MESSAGE(scene_load_from(&fx.pack, id, &why), id);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SCENE_ERR_ASSET, why.status, id);
    TEST_ASSERT_EQUAL_INT_MESSAGE(asset, why.asset, id);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(what, why.what, id);
    TEST_ASSERT_TRUE_MESSAGE(memory_free_bytes(MEMORY_PSRAM) == before, id);
}

static void
test_a_malformed_entry_a_missing_clip_or_track_and_a_mesh_id_fail_naming_the_entry(void) {
    fixture();
    expect_failure(BAD_SCENE, ASSET_ERR_FORMAT, BAD_SCENE);
    expect_failure("test_lost", ASSET_ERR_NOT_FOUND, "nowhere");
    expect_failure("test_headless", ASSET_ERR_NOT_FOUND, "flight");
    expect_failure("test_skewed", ASSET_ERR_FORMAT, "skewed");
    expect_failure("test_unturned", ASSET_ERR_NOT_FOUND, "unturned");
    expect_failure("red", ASSET_ERR_TYPE, "red"); /* a mesh, not a scene */
    TEST_ASSERT_EQUAL_INT(0, scene_loaded_count());
}

static void
test_a_scene_gives_each_entity_s_mesh_id_and_each_camera_s_lens(void) {
    fixture();
    scene_t* pair = load("test_pair");
    TEST_ASSERT_EQUAL_STRING("green", scene_entity_mesh_id(pair, scene_find(pair, "green")));
    TEST_ASSERT_NULL(scene_entity_mesh_id(pair, scene_find(pair, "camera")));
    TEST_ASSERT_NOT_NULL(scene_camera_lens(pair, "camera"));
    TEST_ASSERT_NULL(scene_camera_lens(pair, "red"));
    TEST_ASSERT_EQUAL_UINT32(0, r3d_scene_camera_period_ms(scene_camera_lens(pair, NULL)));

    const r3d_scene_camera_t* flight = scene_camera_lens(load("test_flight"), NULL);
    TEST_ASSERT_EQUAL_UINT32(1000, r3d_scene_camera_period_ms(flight));
    vec3f_t eye;
    vec3f_t forward;
    r3d_scene_camera_sample(flight, 500, &eye, &forward);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, eye.x);
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, forward.z);
}

/* Where the pair's entry puts its parts, as the pack holds it. */
enum { PAIR_NAMES = 24, PAIR_RENDERERS = 264, PAIR_CAMERA = 336, PAIR_BYTES = 416 };

/* One edit that breaks the pair's entry: `width` bytes of `value` at `at`,
 * little-endian, or with `fill` every byte of a 32-byte field. */
typedef struct {
    const char* what;
    uint32_t at, width, value;
    bool fill;
    asset_status_t want;
} breakage_t;

static const breakage_t BREAKAGES[] = {
    {"another version", 0, 2, 2, false, ASSET_ERR_VERSION},
    {"names not 4-aligned", 8, 4, PAIR_NAMES + 2, false, ASSET_ERR_BOUNDS},
    {"names inside the header", 8, 4, 20, false, ASSET_ERR_BOUNDS},
    {"the camera past the end by part of a row", 20, 4, PAIR_CAMERA + 4, false, ASSET_ERR_BOUNDS},
    {"a name with no NUL", PAIR_NAMES, 1, 'x', true, ASSET_ERR_FORMAT},
    {"bytes after a name's NUL", PAIR_NAMES + 20, 1, 'x', false, ASSET_ERR_FORMAT},
    {"an empty entity name", PAIR_NAMES, 1, 0, true, ASSET_ERR_FORMAT},
    {"an empty mesh id", PAIR_RENDERERS + 4, 1, 0, true, ASSET_ERR_FORMAT},
    {"a renderer's padding in use", PAIR_RENDERERS + 2, 2, 1, false, ASSET_ERR_FORMAT},
    {"a camera of no entity", PAIR_CAMERA, 2, 3, false, ASSET_ERR_FORMAT},
    {"a camera's padding in use", PAIR_CAMERA + 2, 2, 1, false, ASSET_ERR_FORMAT},
    {"a field of view of 0", PAIR_CAMERA + 4, 4, 0, false, ASSET_ERR_FORMAT},
    {"a near plane that is not a number", PAIR_CAMERA + 8, 4, 0x7FC00000U, false, ASSET_ERR_FORMAT},
    {"an infinite field of view", PAIR_CAMERA + 4, 4, 0x7F800000U, false, ASSET_ERR_FORMAT},
    {"a clear colour past 0xFFFFFF", PAIR_CAMERA + 12, 4, 0x1000000U, false, ASSET_ERR_FORMAT},
    {"a clip with no node", PAIR_CAMERA + 16, 1, 'x', false, ASSET_ERR_FORMAT},
    {"a node with no clip", PAIR_CAMERA + 48, 1, 'x', false, ASSET_ERR_FORMAT},
};

static void
apply(uint8_t* entry, const breakage_t* b) {
    if (b->fill) {
        memset(entry + b->at, (int)b->value, ASSET_NAME_MAX);
        return;
    }
    for (uint32_t i = 0; i < b->width; i++) {
        entry[b->at + i] = (uint8_t)(b->value >> (8 * i));
    }
}

static asset_status_t
open_entry(const uint8_t* entry, uint32_t size) {
    scene_asset_t asset;
    return scene_asset_open((asset_view_t){entry, size}, &asset);
}

/* Each edit alone is refused with its status; the entry as written opens,
 * and a load of it fails taking nothing. */
static void
test_the_reader_refuses_an_entry_that_breaks_any_one_rule(void) {
    void* raw = NULL;
    uint8_t* entry = test_alloc_aligned(PAIR_BYTES + 4, 4, &raw);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT32(PAIR_BYTES, scene_entry_size(&SCENES[0]));
    for (size_t i = 0; i < sizeof BREAKAGES / sizeof BREAKAGES[0]; i++) {
        memset(entry, 0, PAIR_BYTES);
        make_scene_entry(entry, &SCENES[0]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, open_entry(entry, PAIR_BYTES), BREAKAGES[i].what);
        apply(entry, &BREAKAGES[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BREAKAGES[i].want, open_entry(entry, PAIR_BYTES), BREAKAGES[i].what);
    }
    memset(entry, 0, PAIR_BYTES + 4);
    make_scene_entry(entry, &SCENES[0]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_BOUNDS, open_entry(entry, 23), "shorter than its header");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_BOUNDS, open_entry(entry, PAIR_BYTES - 1), "a byte short");
    memmove(entry + 1, entry, PAIR_BYTES);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_BOUNDS, open_entry(entry + 1, PAIR_BYTES), "not 4-aligned");
    test_free_aligned(raw);
}

#ifndef DEVICE_BUILD
/* The scene tools/tests/scene_probe.py bakes with the tools' own writer,
 * loaded beside quads for its two meshes: what the scene file says arrives. */
static void
test_a_scene_the_tools_bake_loads_as_its_file_says(void) {
    fixture();
    const char* path = getenv("AUTANA_SCENE_PROBE");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "AUTANA_SCENE_PROBE names the pack scene_probe.py wrote");
    asset_pack_t probe;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(path, &probe, &buffer));
    asset_view_t scene_entry;
    asset_view_t clip_entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&probe, "probe_scene", SCENE_ASSET, &scene_entry));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&probe, "probe", ANIM_TRACKS_ASSET, &clip_entry));
    test_pack_t built = test_pack_begin(fx.bytes, PACK_MAX, 4);
    make_quad_entry(test_pack_add(&built, "red", R3D_LIT_MESH_ASSET, QUAD_BYTES), 255, 0, 0);
    make_quad_entry(test_pack_add(&built, "green", R3D_LIT_MESH_ASSET, QUAD_BYTES), 0, 255, 0);
    memcpy(test_pack_add(&built, "probe_scene", SCENE_ASSET, scene_entry.size), scene_entry.data, scene_entry.size);
    memcpy(test_pack_add(&built, "probe", ANIM_TRACKS_ASSET, clip_entry.size), clip_entry.data, clip_entry.size);
    asset_file_release(buffer);
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, fx.bytes, test_pack_finish(&built)));

    scene_t* scene = scene_load_from(&pack, "probe_scene", NULL);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_EQUAL_UINT16(0, scene_find(scene, "red"));
    TEST_ASSERT_EQUAL_UINT16(1, scene_find(scene, "camera"));
    TEST_ASSERT_EQUAL_UINT16(2, scene_find(scene, "green"));
    TEST_ASSERT_EQUAL_STRING("red", scene_entity_mesh_id(scene, 0));
    TEST_ASSERT_EQUAL_STRING("green", scene_entity_mesh_id(scene, 2));
    TEST_ASSERT_NULL(scene_entity_mesh_id(scene, 1));
    /* a quarter turn about y, scaled (2, 4, 0.5), at (1, 2, 3) */
    const scene_transform_t want = {{{0, 0, 0.5F}, {0, 4, 0}, {-2, 0, 0}}, {1, 2, 3}};
    const scene_transform_t* got = scene_entity_transform(scene, 0);
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
            TEST_ASSERT_FLOAT_WITHIN(1e-6F, want.m[row][column], got->m[row][column]);
        }
    }
    TEST_ASSERT_EQUAL_FLOAT(want.position.x, got->position.x);
    TEST_ASSERT_EQUAL_FLOAT(want.position.y, got->position.y);
    TEST_ASSERT_EQUAL_FLOAT(want.position.z, got->position.z);
    const r3d_scene_camera_t* lens = scene_camera_lens(scene, "camera");
    TEST_ASSERT_EQUAL_FLOAT(0.75F, lens->half_fov_short_tan);
    TEST_ASSERT_EQUAL_FLOAT(1.5F, lens->near_z);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x336699), scene->cameras[0].clear);
    TEST_ASSERT_EQUAL_UINT32(3000, r3d_scene_camera_period_ms(lens)); /* the probe clip's length */
}
#endif

#ifndef DEVICE_BUILD
/* The fixture's pack as pack `name` in the working directory, one byte
 * after its header flipped when `damage` is set. */
static void
write_pack(const char* name, bool damage) {
    char path[64];
    (void)snprintf(path, sizeof path, "./%s.apak", name);
    fx.bytes[ASSET_PACK_HEADER_SIZE] ^= damage ? 1U : 0U;
    test_write_file(path, fx.bytes, fx.pack.size);
    fx.bytes[ASSET_PACK_HEADER_SIZE] ^= damage ? 1U : 0U;
}

static void
remove_pack(const char* name) {
    char path[64];
    (void)snprintf(path, sizeof path, "./%s.apak", name);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

/* scene_load() reads the pack named after the scene once and holds it while
 * a scene loaded from it stays: damage to the file goes unseen until the last
 * one unloads and the next load reads it again. */
static void
test_scene_load_holds_its_pack_until_the_last_scene_from_it_unloads(void) {
    fixture();
    test_asset_dir_use(".");
    write_pack("test_pair", false);
    scene_failure_t why;
    scene_t* first = scene_load("test_pair", &why);
    TEST_ASSERT_NOT_NULL(first);
    write_pack("test_pair", true);
    scene_t* second = scene_load("test_pair", &why);
    TEST_ASSERT_NOT_NULL(second);
    scene_unload(first);
    scene_unload(second);
    TEST_ASSERT_NULL(scene_load("test_pair", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, why.asset);
    remove_pack("test_pair");
    test_asset_dir_restore();
}

/* A load that fails gives its use of the pack back. */
static void
test_a_scene_that_fails_to_load_does_not_hold_its_pack(void) {
    fixture();
    test_asset_dir_use(".");
    write_pack("test_broken", false);
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load("test_broken", &why));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, why.asset);
    TEST_ASSERT_EQUAL_STRING("gone", why.what); /* a copy: the pack it was read from is gone */
    write_pack("test_broken", true);
    TEST_ASSERT_NULL(scene_load("test_broken", &why));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_NO_PACK, why.asset, "the damaged file was read again");
    remove_pack("test_broken");
    test_asset_dir_restore();
}
#endif

static void
test_a_success_clears_what_an_earlier_failure_said(void) {
    fixture();
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_broken", &why));
    TEST_ASSERT_NOT_NULL(scene_load_from(&fx.pack, "test_pair", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_OK, why.status);
    TEST_ASSERT_EQUAL_STRING("test_pair", why.what);
}

static void
test_a_load_with_no_memory_left_says_so_and_takes_nothing(void) {
    fixture();
    scene_failure_t why;
    scene_fail_next_allocation = true;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_pair", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_MEMORY, why.status);
    TEST_ASSERT_NOT_NULL(scene_load_from(&fx.pack, "test_pair", &why)); /* the failure was for one load only */
}

static void
test_a_full_manager_refuses_another_scene_until_one_is_unloaded(void) {
    fixture();
    scene_t* held[8];
    for (int i = 0; i < 8; i++) {
        held[i] = load("test_solo");
    }
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_solo", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_FULL, why.status);
    scene_unload(held[3]);
    TEST_ASSERT_NOT_NULL(scene_load_from(&fx.pack, "test_solo", &why));
}

static void
test_an_entity_is_found_by_its_name(void) {
    fixture();
    scene_t* scene = load("test_pair");
    TEST_ASSERT_EQUAL_UINT16(1, scene_find(scene, "red"));
    TEST_ASSERT_EQUAL_UINT16(2, scene_find(scene, "green"));
    TEST_ASSERT_EQUAL_UINT16(SCENE_ENTITY_NONE, scene_find(scene, "yellow"));
    TEST_ASSERT_EQUAL_FLOAT(4.0F, scene_entity_transform(scene, scene_find(scene, "green"))->position.x);
}

static void
test_the_active_camera_draws_its_scenes_renderers(void) {
    fixture();
    show("test_pair", NULL);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x00FF00), pixel(4.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-4.0F));
}

static void
test_a_disabled_entity_is_not_drawn_and_enabling_it_draws_it_again(void) {
    fixture();
    scene_t* scene = show("test_pair", NULL);
    scene_entity_set_enabled(scene, scene_find(scene, "green"), false);
    TEST_ASSERT_FALSE(scene_entity_enabled(scene, scene_find(scene, "green")));
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(4.0F));
    scene_entity_set_enabled(scene, scene_find(scene, "green"), true);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x00FF00), pixel(4.0F));
}

static void
test_a_moved_entity_is_drawn_where_it_now_stands(void) {
    fixture();
    scene_t* scene = show("test_pair", NULL);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
    const scene_transform_t away = AT(-4, 0, 0);
    scene_entity_set_transform(scene, scene_find(scene, "red"), &away);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-4.0F));
    const scene_transform_t home = IDENTITY;
    scene_entity_set_transform(scene, scene_find(scene, "red"), &home);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
}

static void
test_with_nothing_enabled_the_picture_is_left_alone(void) {
    fixture();
    scene_t* scene = show("test_pair", NULL);
    scene_entity_set_enabled(scene, scene_find(scene, "red"), false);
    scene_entity_set_enabled(scene, scene_find(scene, "green"), false);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
}

static void
test_two_scenes_are_held_at_once_and_the_active_camera_decides_which_is_seen(void) {
    fixture();
    scene_t* pair = show("test_pair", NULL);
    scene_t* solo = load("test_solo");
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
    TEST_ASSERT_TRUE(scene_activate(solo, "eye"));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(4.0F)); /* the other scene's quads are not drawn */
    scene_entity_set_enabled(pair, scene_find(pair, "red"), false);
    TEST_ASSERT_TRUE(scene_activate(pair, NULL));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F)); /* each scene kept its own state */
}

static void
test_unloading_the_active_scene_stops_drawing_and_leaves_the_other_alone(void) {
    fixture();
    scene_t* pair = show("test_pair", NULL);
    scene_t* solo = load("test_solo");
    TEST_ASSERT_TRUE(scene_has_active_camera());
    scene_unload(pair);
    TEST_ASSERT_FALSE(scene_has_active_camera());
    TEST_ASSERT_TRUE(scene_activate(solo, NULL));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), pixel(0.0F));
}

static void
test_activating_a_camera_the_scene_does_not_have_changes_nothing(void) {
    fixture();
    scene_t* pair = show("test_pair", NULL);
    TEST_ASSERT_FALSE(scene_activate(pair, "red")); /* an entity, but not a camera */
    TEST_ASSERT_FALSE(scene_activate(pair, "nothing"));
    TEST_ASSERT_TRUE(scene_has_active_camera());
}

static void
test_the_active_camera_flies_its_path_by_the_time_the_shell_gives_it(void) {
    fixture();
    show("test_flight", NULL);
    frame(500);
    /* halfway: 2.5 units along x, so the quad is 2.5 units to the left */
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F));
    frame(250); /* 3.75 units along, the quad further left still */
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-1.0F));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-3.75F));
}

/* The shell draws in scene_render(), while the last frame is being sent, and
 * upscales in scene_compose(): time passes once, and the picture is the one
 * scene_compose() alone would have made. */
static void
test_a_frame_drawn_in_two_steps_is_the_frame_drawn_in_one(void) {
    fixture();
    show("test_flight", NULL);
    scene_render(500, 0, SIZE, SIZE);
    scene_compose(500, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F));
    scene_render(250, 0, SIZE, SIZE);
    scene_compose(250, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-3.75F));
}

static void
test_a_paused_scene_neither_advances_nor_draws_until_resumed(void) {
    fixture();
    show("test_flight", NULL);
    scene_set_paused(true);
    TEST_ASSERT_FALSE(scene_has_active_camera());
    frame(500);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(-2.5F));
    scene_render(500, 0, SIZE, SIZE); /* the shell's overlapped half counts no time either */
    scene_set_paused(false);
    frame(500);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F)); /* the paused 500 ms did not count */
}

static void
test_a_scale_renders_smaller_and_the_picture_is_upscaled_to_the_target(void) {
    fixture();
    show("test_pair", NULL);
    render_context_set_scale(render_context_main(), 50);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x00FF00), pixel(4.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-4.0F));
}

static void
test_the_stats_count_what_the_last_draw_kept(void) {
    fixture();
    scene_t* scene = show("test_pair", NULL);
    frame(16);
    TEST_ASSERT_EQUAL_INT(4,
                          render_context_frame(render_context_main()).stats.triangles); /* two quads of two triangles */
    scene_entity_set_enabled(scene, scene_find(scene, "green"), false);
    frame(16);
    TEST_ASSERT_EQUAL_INT(2, render_context_frame(render_context_main()).stats.triangles);
}

/* An app leaving: nothing it loaded outlives it, and the raster's scratch
 * goes too. */
static void
test_leaving_the_app_unloads_every_scene_and_frees_the_scratch(void) {
    fixture();
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    show("test_pair", NULL);
    load("test_solo");
    frame(16);
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) < before);
    scene_unload_all();
    TEST_ASSERT_FALSE(scene_has_active_camera());
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) == before);
}

/* The draw happens in scene_render(): it touches no framebuffer and leaves
 * the picture to scene_compose(), but the work is done. */
static void
test_scene_render_draws_into_scratch_and_leaves_the_framebuffer_alone(void) {
    fixture();
    show("test_pair", NULL);
    for (int i = 0; i < SIZE * SIZE; i++) {
        fx.pixels[i] = SENTINEL;
    }
    scene_render(16, 0, SIZE, SIZE);
    TEST_ASSERT_EQUAL_INT(4, render_context_frame(render_context_main()).stats.triangles);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
    scene_compose(16, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
}

/* Band mode has no framebuffer: the scene still draws, and the compose that
 * follows has nowhere to upscale to and writes nothing. */
static void
test_a_compose_with_no_framebuffer_writes_nothing(void) {
    fixture();
    show("test_pair", NULL);
    for (int i = 0; i < SIZE * SIZE; i++) {
        fx.pixels[i] = SENTINEL;
    }
    scene_render(16, 0, SIZE, SIZE);
    const scene_target_t none = {NULL, SIZE, SIZE};
    scene_compose(16, 0, &none);
    TEST_ASSERT_EQUAL_INT(4, render_context_frame(render_context_main()).stats.triangles);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
    frame(16); /* and the next frame, with a framebuffer again, is whole */
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
}

static void
test_leaving_an_app_while_paused_lifts_the_pause(void) {
    fixture();
    show("test_pair", NULL);
    scene_set_paused(true);
    scene_unload_all();
    show("test_pair", NULL);
    TEST_ASSERT_TRUE(scene_has_active_camera());
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
}

static void
test_a_scene_with_two_cameras_is_seen_from_the_one_activated_by_name(void) {
    fixture();
    scene_t* twin = load("test_twin");
    TEST_ASSERT_TRUE(scene_activate(twin, "right"));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F)); /* the quad is 4 units left of this camera */
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-4.0F));
    TEST_ASSERT_TRUE(scene_activate(twin, "left"));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
    TEST_ASSERT_TRUE(scene_activate(twin, NULL)); /* the first */
    TEST_ASSERT_FALSE(scene_activate(twin, "red"));
}

static void
test_a_camera_without_a_path_follows_its_entitys_transform(void) {
    fixture();
    scene_t* pair = show("test_pair", "camera");
    const scene_transform_t aside = AT(4, 0, 10);
    scene_entity_set_transform(pair, scene_find(pair, "camera"), &aside);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x00FF00), pixel(0.0F)); /* the green quad is 4 units right of the origin */
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-4.0F));
}

static void
test_the_render_scale_changes_the_picture_and_a_larger_one_grows_the_scratch(void) {
    fixture();
    show("test_pair", NULL);
    render_context_set_scale(render_context_main(), 50);
    frame(16);
    const size_t small_free = memory_free_bytes(MEMORY_PSRAM);
    uint16_t* half = malloc(sizeof(*half) * SIZE * SIZE);
    TEST_ASSERT_NOT_NULL(half);
    memcpy(half, fx.pixels, sizeof(*half) * SIZE * SIZE);
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) < small_free);
    TEST_ASSERT_TRUE(memcmp(half, fx.pixels, sizeof(*half) * SIZE * SIZE) != 0);
    free(half);
}

/* Three steps of the 64-pixel destination, and a model that prices a step by
 * its share of the pixels alone: 1000, 562 and 250 microseconds. */
static const resolution_step_t LADDER[3] = {{SIZE, SIZE}, {SIZE * 3 / 4, SIZE * 3 / 4}, {SIZE / 2, SIZE / 2}};
static const resolution_model_t PIXEL_MODEL = {.per_pixel_share_us = 1000.0F};

/* The frame as the predictor draws it: opting in afresh puts the predictor on
 * `first`, so the census is taken there, and the budget picks the step. */
static void
predicted_frame(int first, int32_t budget_us) {
    const resolution_config_t config = resolution_config(LADDER, 3, 3, budget_us);
    render_context_set_dynamic_resolution(render_context_main(), &config, &PIXEL_MODEL, first);
    frame(0);
}

/* A draw is censused where the predictor stood and drawn where it chose, from
 * the list the census kept: finer, coarser or two steps away, the picture and
 * the stats are those of a draw at the chosen size alone. */
static void
test_a_predicted_draw_is_the_fixed_draw_at_the_step_it_chose(void) {
    fixture();
    show("test_pair", NULL);

    const struct {
        int first;
        int32_t budget_us;
        int chosen;
    } cases[] = {{0, 700, 1}, {2, 700, 1}, {0, 300, 2}, {2, 2000, 0}, {1, 2000, 0}, {1, 300, 2}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        predicted_frame(cases[i].first, cases[i].budget_us);
        const render_context_frame_t drawn = render_context_frame(render_context_main());
        TEST_ASSERT_EQUAL_INT(cases[i].chosen, drawn.step);
        TEST_ASSERT_EQUAL_INT(LADDER[cases[i].chosen].width, drawn.width);
        memcpy(fx.held, fx.pixels, sizeof(*fx.held) * SIZE * SIZE);

        render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
        render_context_set_scale(render_context_main(), 100 * LADDER[cases[i].chosen].width / SIZE);
        frame(0);
        const render_context_frame_t fixed = render_context_frame(render_context_main());
        TEST_ASSERT_EQUAL_INT(drawn.width, fixed.width);
        TEST_ASSERT_EQUAL_INT(fixed.stats.clusters, drawn.stats.clusters);
        TEST_ASSERT_EQUAL_INT(fixed.stats.triangles, drawn.stats.triangles);
        TEST_ASSERT_EQUAL_INT(4, drawn.stats.triangles);
        TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(0.0F));
        TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, fx.pixels, sizeof(*fx.held) * SIZE * SIZE));
    }
}

/* The scratch block is taken once, for the finest step and the list the
 * census keeps, wherever the predictor stands when it is taken. */
static void
test_the_scratch_holds_the_finest_step_and_the_culled_list(void) {
    fixture();
    scene_t* scene = show("test_pair", NULL);
    r3d_instance_t* instances = fx.instances;
    for (int i = 0; i < INSTANCES_MAX; i++) {
        instances[i] = (r3d_instance_t){&scene->renderers[0].mesh, NULL};
    }
    const resolution_config_t config = resolution_config(LADDER, 3, 3, 2000);
    render_context_t* c = render_context_main();
    render_context_set_dynamic_resolution(c, &config, &PIXEL_MODEL, 2);
    const camera_t view = r3d_scene_camera_at(&scene->cameras[0].lens, 0);
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_TRUE(render_context_draw(c, instances, INSTANCES_MAX, &view, 0, 0, SIZE, SIZE));
    const size_t taken = before - memory_free_bytes(MEMORY_PSRAM);

    raster_t finest = c->raster;
    finest.width = LADDER[0].width;
    finest.height = LADDER[0].height;
    const size_t needed = raster_scratch_bytes(&finest) + (sizeof(uint16_t) * raster_culled_length(&finest));
    TEST_ASSERT_TRUE(taken >= needed);
    TEST_ASSERT_TRUE(c->scratch_bytes == needed);
}

static void
test_unloading_the_first_of_two_scenes_leaves_the_second_with_its_own_time(void) {
    fixture();
    scene_t* pair = load("test_pair");
    scene_t* flight = show("test_flight", NULL);
    frame(250);
    scene_unload(pair);
    TEST_ASSERT_EQUAL_INT(1, scene_loaded_count());
    TEST_ASSERT_TRUE(scene_loaded_at(0) == flight);
    frame(250);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F)); /* 500 ms of flight */
}

static void
test_a_loaded_scene_that_is_not_active_keeps_its_time(void) {
    fixture();
    scene_t* flight = load("test_flight");
    show("test_pair", NULL);
    frame(500);
    TEST_ASSERT_TRUE(scene_activate(flight, NULL));
    render_context_set_scale(render_context_main(), 100);
    frame(0);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F)); /* it flew while another was seen */
}

void
run_scene_suite(void) {
    RUN_TEST(test_a_scene_loads_by_name_and_unloading_gives_back_everything_it_took);
    RUN_TEST(test_a_camera_clears_to_the_colour_its_scene_gives);
    RUN_TEST(test_a_load_that_fails_says_what_it_was_about);
    RUN_TEST(test_a_success_clears_what_an_earlier_failure_said);
    RUN_TEST(test_a_malformed_entry_a_missing_clip_or_track_and_a_mesh_id_fail_naming_the_entry);
    RUN_TEST(test_a_scene_gives_each_entity_s_mesh_id_and_each_camera_s_lens);
    RUN_TEST(test_the_reader_refuses_an_entry_that_breaks_any_one_rule);
    RUN_TEST(test_an_id_too_long_for_a_pack_name_is_cut_where_the_failure_names_it);
#ifndef DEVICE_BUILD
    RUN_TEST(test_a_scene_the_tools_bake_loads_as_its_file_says);
#endif
#ifndef DEVICE_BUILD
    RUN_TEST(test_scene_load_holds_its_pack_until_the_last_scene_from_it_unloads);
    RUN_TEST(test_a_scene_that_fails_to_load_does_not_hold_its_pack);
#endif
    RUN_TEST(test_a_load_with_no_memory_left_says_so_and_takes_nothing);
    RUN_TEST(test_a_full_manager_refuses_another_scene_until_one_is_unloaded);
    RUN_TEST(test_an_entity_is_found_by_its_name);
    RUN_TEST(test_the_active_camera_draws_its_scenes_renderers);
    RUN_TEST(test_a_disabled_entity_is_not_drawn_and_enabling_it_draws_it_again);
    RUN_TEST(test_a_moved_entity_is_drawn_where_it_now_stands);
    RUN_TEST(test_with_nothing_enabled_the_picture_is_left_alone);
    RUN_TEST(test_two_scenes_are_held_at_once_and_the_active_camera_decides_which_is_seen);
    RUN_TEST(test_unloading_the_active_scene_stops_drawing_and_leaves_the_other_alone);
    RUN_TEST(test_activating_a_camera_the_scene_does_not_have_changes_nothing);
    RUN_TEST(test_the_active_camera_flies_its_path_by_the_time_the_shell_gives_it);
    RUN_TEST(test_a_frame_drawn_in_two_steps_is_the_frame_drawn_in_one);
    RUN_TEST(test_a_paused_scene_neither_advances_nor_draws_until_resumed);
    RUN_TEST(test_a_scale_renders_smaller_and_the_picture_is_upscaled_to_the_target);
    RUN_TEST(test_the_stats_count_what_the_last_draw_kept);
    RUN_TEST(test_leaving_the_app_unloads_every_scene_and_frees_the_scratch);
    RUN_TEST(test_scene_render_draws_into_scratch_and_leaves_the_framebuffer_alone);
    RUN_TEST(test_a_compose_with_no_framebuffer_writes_nothing);
    RUN_TEST(test_leaving_an_app_while_paused_lifts_the_pause);
    RUN_TEST(test_a_scene_with_two_cameras_is_seen_from_the_one_activated_by_name);
    RUN_TEST(test_a_camera_without_a_path_follows_its_entitys_transform);
    RUN_TEST(test_the_render_scale_changes_the_picture_and_a_larger_one_grows_the_scratch);
    RUN_TEST(test_unloading_the_first_of_two_scenes_leaves_the_second_with_its_own_time);
    RUN_TEST(test_a_loaded_scene_that_is_not_active_keeps_its_time);
    RUN_TEST(test_a_camera_keeps_its_fixed_scale_unless_dynamic_resolution_is_asked_for);
    RUN_TEST(test_a_predicted_draw_is_the_fixed_draw_at_the_step_it_chose);
    RUN_TEST(test_the_scratch_holds_the_finest_step_and_the_culled_list);
}

SUITE_REGISTER(run_scene_suite);
