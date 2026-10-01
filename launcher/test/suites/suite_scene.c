/*
 * Portable suite: the scene manager (scene/scene.h). Each test loads scenes
 * from a small pack built here, of unit quads in distinct colours, so what
 * was drawn is read straight off the picture: a pixel is a quad's colour or
 * the clear colour. The camera stands at z = 10 with a half field of view of
 * 1, which puts a unit of x 3.2 pixels from the centre of a 64-pixel picture.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "esp_heap_caps.h"

#include "asset/asset_pack.h"
#include "gfx/gfx_color.h"
#include "scene/scene.h"
#include "test_cleanup.h"

#define SIZE       64
#define CENTER     (SIZE / 2)
#define CLEAR_RGB  0x336699
#define ENTRY_SIZE 132
#define ENTRY_STEP 144 /* the entry rounded up to the 16 bytes the pack aligns to */
#define PACK_MAX   640
#define SENTINEL   0x5A5A

static void
put32(uint8_t* at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = (uint8_t)(value >> (8 * i));
    }
}

static void
put16(uint8_t* at, int value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

/* A unit quad in the plane z = 0, two-sided, in one colour. The arrays sit
 * one after another after the 44-byte header. */
static void
make_quad_entry(uint8_t* entry, uint8_t red, uint8_t green, uint8_t blue) {
    enum { POSITIONS = 44, COLORS = 68, TRIANGLES = 80, CLUSTERS = 92, NODES = 116 };

    memset(entry, 0, ENTRY_SIZE);
    const uint32_t words[11] = {4, 2, 1, 1, 1, POSITIONS, COLORS, TRIANGLES, CLUSTERS, NODES, 0};
    for (int i = 0; i < 11; i++) {
        put32(entry + (4 * i), words[i]);
    }
    const int xs[4] = {-1, 1, 1, -1};
    const int ys[4] = {-1, -1, 1, 1};
    for (int v = 0; v < 4; v++) {
        put16(entry + POSITIONS + (6 * v), xs[v]);
        put16(entry + POSITIONS + (6 * v) + 2, ys[v]);
        entry[COLORS + (3 * v)] = red;
        entry[COLORS + (3 * v) + 1] = green;
        entry[COLORS + (3 * v) + 2] = blue;
    }
    const int corners[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        put16(entry + TRIANGLES + (2 * i), corners[i]);
    }
    put16(entry + CLUSTERS + 2, 4);   /* vertex_count */
    put16(entry + CLUSTERS + 6, 2);   /* triangle_count */
    put16(entry + CLUSTERS + 8, -1);  /* lo x */
    put16(entry + CLUSTERS + 10, -1); /* lo y */
    put16(entry + CLUSTERS + 14, 1);  /* hi x */
    put16(entry + CLUSTERS + 16, 1);  /* hi y */
    entry[CLUSTERS + 20] = 1;         /* double sided */
    put16(entry + NODES, -1);
    put16(entry + NODES + 2, -1);
    put16(entry + NODES + 6, 1);
    put16(entry + NODES + 8, 1);
    entry[NODES + 14] = 1; /* count */
    entry[NODES + 15] = 1; /* leaf */
}

static const struct {
    const char* name;
    uint8_t red, green, blue;
} QUADS[] = {{"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}};

#define QUAD_COUNT 3

/* The pack of every quad, with the CRC set. */
static uint32_t
make_pack(uint8_t* pack) {
    const uint32_t first = 192; /* 32 header, 3 table rows of 48, rounded to 16 */
    memset(pack, 0, PACK_MAX);
    memcpy(pack, ASSET_PACK_MAGIC, 4);
    put32(pack + 4, ASSET_PACK_VERSION);
    put32(pack + 8, QUAD_COUNT);
    for (uint32_t i = 0; i < QUAD_COUNT; i++) {
        uint8_t* row = pack + ASSET_PACK_HEADER_SIZE + (i * ASSET_PACK_ENTRY_SIZE);
        memcpy(row, QUADS[i].name, strlen(QUADS[i].name) + 1);
        put32(row + 32, R3D_LIT_MESH_ASSET);
        put32(row + 36, first + (i * ENTRY_STEP));
        put32(row + 40, ENTRY_SIZE);
        put32(row + 44, 16);
        make_quad_entry(pack + first + (i * ENTRY_STEP), QUADS[i].red, QUADS[i].green, QUADS[i].blue);
    }
    const uint32_t total = first + (QUAD_COUNT * ENTRY_STEP);
    put32(pack + 12, total);
    put32(pack + 16, asset_crc32(pack + ASSET_PACK_HEADER_SIZE, total - ASSET_PACK_HEADER_SIZE));
    return total;
}

#define IDENTITY {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0, 0, 0}}
#define AT(x, y, z)                                                                                                    \
    {                                                                                                                  \
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, { x, y, z }                                                                 \
    }

/* "pair": a camera, a red quad at the origin and a green one 4 units right. */
static const char* const PAIR_NAMES[] = {"camera", "red", "green"};
static const scene_transform_t PAIR_TRANSFORMS[] = {AT(0, 0, 10), IDENTITY, AT(4, 0, 0)};
static const scene_renderer_def_t PAIR_RENDERERS[] = {{1, "red"}, {2, "green"}};
static const scene_camera_def_t PAIR_CAMERAS[] = {{0, 1.0F, 1.0F, NULL}};
static const scene_def_t PAIR = {"test_pair", 3, 2, 1, PAIR_NAMES, PAIR_TRANSFORMS, PAIR_RENDERERS, PAIR_CAMERAS};
SCENE_REGISTER(PAIR)

/* "solo": its own camera and one blue quad at the origin. */
static const char* const SOLO_NAMES[] = {"eye", "blue"};
static const scene_transform_t SOLO_TRANSFORMS[] = {AT(0, 0, 10), IDENTITY};
static const scene_renderer_def_t SOLO_RENDERERS[] = {{1, "blue"}};
static const scene_camera_def_t SOLO_CAMERAS[] = {{0, 1.0F, 1.0F, NULL}};
static const scene_def_t SOLO = {"test_solo", 2, 1, 1, SOLO_NAMES, SOLO_TRANSFORMS, SOLO_RENDERERS, SOLO_CAMERAS};
SCENE_REGISTER(SOLO)

/* "broken": names a mesh the pack does not hold. */
static const char* const BROKEN_NAMES[] = {"red", "gone"};
static const scene_transform_t BROKEN_TRANSFORMS[] = {IDENTITY, IDENTITY};
static const scene_renderer_def_t BROKEN_RENDERERS[] = {{0, "red"}, {1, "gone"}};
static const scene_def_t BROKEN = {"test_broken", 2, 2, 0, BROKEN_NAMES, BROKEN_TRANSFORMS, BROKEN_RENDERERS, NULL};
SCENE_REGISTER(BROKEN)

/* "flight": a camera that flies 5 units along x in a second, and a red quad. */
static const float FLIGHT_TIMES[] = {0.0F, 1.0F};
static const float FLIGHT_POSITIONS[] = {0, 0, 10, 5, 0, 10};
static const float FLIGHT_TURNS[] = {0, 0, 0, 1, 0, 0, 0, 1};
static const anim_track_t FLIGHT_TRANSLATION = {FLIGHT_TIMES, FLIGHT_POSITIONS, 2, 3, ANIM_LINEAR, 0};
static const anim_track_t FLIGHT_ROTATION = {FLIGHT_TIMES, FLIGHT_TURNS, 2, 4, ANIM_LINEAR, 1};
static const anim_track_t* const FLIGHT_TRACKS[] = {&FLIGHT_TRANSLATION, &FLIGHT_ROTATION};
static const anim_clip_t FLIGHT_CLIP = {FLIGHT_TRACKS, 2, 1000};
static const r3d_scene_path_t FLIGHT_PATH = {&FLIGHT_CLIP, &FLIGHT_TRANSLATION, &FLIGHT_ROTATION};
static const char* const FLIGHT_NAMES[] = {"camera", "red"};
static const scene_transform_t FLIGHT_TRANSFORMS[] = {IDENTITY, IDENTITY};
static const scene_renderer_def_t FLIGHT_RENDERERS[] = {{1, "red"}};
static const scene_camera_def_t FLIGHT_CAMERAS[] = {{0, 1.0F, 1.0F, &FLIGHT_PATH}};
static const scene_def_t FLIGHT = {"test_flight", 2, 1, 1, FLIGHT_NAMES, FLIGHT_TRANSFORMS, FLIGHT_RENDERERS,
                                   FLIGHT_CAMERAS};
SCENE_REGISTER(FLIGHT)

typedef struct {
    uint8_t* bytes;
    uint16_t* pixels;
    asset_pack_t pack;
    scene_target_t target;
} fixture_t;

static fixture_t fx;

static void
release_fixture(void) {
    scene_unload_all();
    free(fx.bytes);
    free(fx.pixels);
    fx = (fixture_t){0};
}

/* Every test starts from an empty engine, so the order of tests is no one's
 * business. */
static void
fixture(void) {
    scene_unload_all();
    fx.bytes = malloc(PACK_MAX);
    fx.pixels = malloc(sizeof(*fx.pixels) * SIZE * SIZE);
    TEST_ASSERT_NOT_NULL(fx.bytes);
    TEST_ASSERT_NOT_NULL(fx.pixels);
    suite_set_test_cleanup(release_fixture);
    const uint32_t total = make_pack(fx.bytes);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&fx.pack, fx.bytes, total));
    fx.target = (scene_target_t){fx.pixels, SIZE, SIZE};
}

static scene_t*
load(const char* name) {
    scene_t* scene = scene_load_from(&fx.pack, name);
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, name);
    return scene;
}

/* Loads, activates the camera at full size and clears to CLEAR_RGB. */
static scene_t*
show(const char* name, const char* camera) {
    scene_t* scene = load(name);
    TEST_ASSERT_TRUE(scene_activate(scene, camera));
    scene_set_render_scale(100);
    scene_set_clear(CLEAR_RGB);
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
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    scene_t* scene = load("test_pair");
    TEST_ASSERT_TRUE(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < before);
    scene_unload(scene);
    TEST_ASSERT_TRUE(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == before);
}

static void
test_a_load_that_fails_says_what_it_was_about(void) {
    fixture();
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_nothing_of_the_kind"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, scene_load_failure().status);
    TEST_ASSERT_EQUAL_STRING("test_nothing_of_the_kind", scene_load_failure().what);

    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_broken"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, scene_load_failure().status);
    TEST_ASSERT_EQUAL_STRING("gone", scene_load_failure().what);
    TEST_ASSERT_TRUE(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == before);

    TEST_ASSERT_NULL(scene_load_from(NULL, "test_pair"));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, scene_load_failure().status);
    TEST_ASSERT_EQUAL_STRING("red", scene_load_failure().what);
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
    scene_set_render_scale(100);
    scene_set_clear(CLEAR_RGB);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(4.0F)); /* the other scene's quads are not drawn */
    scene_entity_set_enabled(pair, scene_find(pair, "red"), false);
    TEST_ASSERT_TRUE(scene_activate(pair, NULL));
    scene_set_render_scale(100);
    scene_set_clear(CLEAR_RGB);
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
    scene_set_render_scale(100);
    scene_set_clear(CLEAR_RGB);
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
    scene_render(500, 0, &fx.target);
    scene_compose(500, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F));
    scene_render(250, 0, &fx.target);
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
    scene_set_paused(false);
    frame(500);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0xFF0000), pixel(-2.5F)); /* the paused 500 ms did not count */
}

static void
test_a_scale_renders_smaller_and_the_picture_is_upscaled_to_the_target(void) {
    fixture();
    show("test_pair", NULL);
    scene_set_render_scale(50);
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
    TEST_ASSERT_EQUAL_INT(4, scene_stats().triangles); /* two quads of two triangles */
    scene_entity_set_enabled(scene, scene_find(scene, "green"), false);
    frame(16);
    TEST_ASSERT_EQUAL_INT(2, scene_stats().triangles);
}

/* An app leaving: nothing it loaded outlives it, and the raster's scratch
 * goes too. */
static void
test_leaving_the_app_unloads_every_scene_and_frees_the_scratch(void) {
    fixture();
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    show("test_pair", NULL);
    load("test_solo");
    frame(16);
    TEST_ASSERT_TRUE(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < before);
    scene_unload_all();
    TEST_ASSERT_FALSE(scene_has_active_camera());
    TEST_ASSERT_TRUE(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == before);
}

void
run_scene_suite(void) {
    RUN_TEST(test_a_scene_loads_by_name_and_unloading_gives_back_everything_it_took);
    RUN_TEST(test_a_load_that_fails_says_what_it_was_about);
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
}

SUITE_REGISTER(run_scene_suite);
