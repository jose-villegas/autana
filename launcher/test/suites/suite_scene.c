/*
 * Portable scene-manager tests use small packs with distinct quad colours
 * so pixels identify the drawn entry. Each camera stands CAMERA_DISTANCE in
 * front of the quads' plane with a half-field tangent of 1, so a unit across
 * is PIXELS_PER_UNIT pixels from the centre.
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "anim/anim_tracks_format.h"
#include "hash/crc32.h"
#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#include "anim/anim_tracks.h"
#include "asset/asset_pack.h"
#include "gfx/target/gfx_color.h"
#ifndef DEVICE_BUILD
#include "display/display.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "shell/shell_frame.h"
#include "ui/ui.h"
#endif
#include "core/memory.h"
#ifdef HOST_HEAP_ARENA
#include "job_calls.h"
#endif
#include "lcol_name_vectors.h"
#include "math/scalar/mathi.h"
#include "render/context/render_context.h"
#include "render/lcol_format.h"
#include "scene/scene.h"
#include "scene/scene_internal.h"
#include "scene/scene_shell.h"
#include "test_alloc.h"
#include "test_anim_tracks.h"
#include "test_pack.h"
#include "testkit/cleanup.h"

#ifndef DEVICE_BUILD
#include <stdio.h>

#include "../scene_examples/animator.c"
#include "../scene_examples/hall.c"
#include "../scene_examples/present.c"
#include "../scene_examples/small_fill.c"
#include "anim/anim_skeleton_format.h"
#include "asset/asset_file.h"
#include "image/pixel/rgb565.h"
#include "render/r3d_skin_format.h"
#include "test_asset_dir.h"
#include "ui/ui_internal.h"
#endif

#define SIZE               64
#define CENTER             (SIZE / 2)
#define PICTURE_HEIGHT     (SIZE / 2)
#define GREEN_PIXEL_OFFSET 6
#define CAMERA_DISTANCE    10
#define PIXELS_PER_UNIT    ((float)CENTER / CAMERA_DISTANCE)
#define CLEAR_RGB          0x336699
#define QUAD_BYTES         132
#define PACK_MAX           16384
#define SENTINEL           0x5A5A

/* Copies of one mesh a test draws, to make a culled list worth measuring. */
#define INSTANCES_MAX      64

/* A unit quad in the plane z = 0, two-sided, in one colour. The arrays sit
 * one after another after the 44-byte header. */
enum {
    QUAD_VERTICES = 4,
    QUAD_TRIANGLES = 2,
    MESH_HEADER = 44,
    MESH_SCALE_AT = 4 * sizeof(uint32_t),
    RIG_JOINTS = 4,
    RIG_ENTRIES = 10,
    RIG_MESH_BYTES = 200,
    RIG_VERTICES = 2 * QUAD_VERTICES,
    RIG_DURATION_MS = 1000,
    RIG_TRAVEL = 4
};

static void
make_quad_entry(uint8_t* entry, uint8_t red, uint8_t green, uint8_t blue, int copies) {
    const int vertices = QUAD_VERTICES * copies, triangles = QUAD_TRIANGLES * copies;
    const uint32_t positions = MESH_HEADER, colors = positions + vertices * sizeof(int16_t[3]);
    const uint32_t corners_at = colors + vertices * sizeof(uint8_t[3]);
    const uint32_t clusters = corners_at + triangles * sizeof(uint16_t[3]);
    const uint32_t nodes = mathi_size_ceil(clusters + copies * sizeof(r3d_lit_cluster_t), sizeof(uint32_t));
    const uint32_t words[] = {vertices, triangles, copies, 1, 1, positions, colors, corners_at, clusters, nodes, 0};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        test_pack_put32(entry + sizeof(uint32_t) * i, words[i]);
    }
    static const int16_t points[QUAD_VERTICES][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    static const uint16_t corners[QUAD_TRIANGLES][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int c = 0; c < copies; c++) {
        memcpy(entry + positions + c * sizeof points, points, sizeof points);
        for (int v = c * QUAD_VERTICES; v < (c + 1) * QUAD_VERTICES; v++) {
            const uint8_t color[] = {red, green, blue};
            memcpy(entry + colors + v * sizeof color, color, sizeof color);
        }
        for (int t = 0; t < QUAD_TRIANGLES; t++) {
            for (int axis = 0; axis < 3; axis++) {
                test_pack_put16(entry + corners_at + (c * QUAD_TRIANGLES + t) * sizeof(uint16_t[3])
                                    + axis * sizeof(uint16_t),
                                corners[t][axis] + c * QUAD_VERTICES);
            }
        }
        const r3d_lit_cluster_t cluster = {c * QUAD_VERTICES,        QUAD_VERTICES, c * QUAD_TRIANGLES, QUAD_TRIANGLES,
                                           {{-1, -1, 0}, {1, 1, 0}}, true};
        memcpy(entry + clusters + c * sizeof cluster, &cluster, sizeof cluster);
    }
    const r3d_lit_node_t node = {{{-1, -1, 0}, {1, 1, 0}}, 0, copies, true};
    memcpy(entry + nodes, &node, sizeof node);
}

static const struct {
    const char* name;
    uint8_t red, green, blue;
} QUADS[] = {{"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}};

#define QUAD_COUNT 3

/* A clip of the camera node flying 5 units along x in a second, facing down
 * +z. Both tracks share the times; each array starts where the last ends. */
enum {
    FLIGHT_TIMES = TEST_TRACKS_HEADER_SIZE + (2 * (TEST_TRACK_ROW_SIZE + TEST_TRACK_STRING_SLOT)),
    FLIGHT_POSITIONS = FLIGHT_TIMES + (2 * sizeof(float)),
    FLIGHT_TURNS = FLIGHT_POSITIONS + (6 * sizeof(float)),
    FLIGHT_BYTES = FLIGHT_TURNS + (8 * sizeof(float))
};

/* A camera translation must be three-wide; unkeyed rotation retains its value. */
static const struct {
    const char* id;
    anim_value_t position_type;
    int track_count;
} CLIPS[] = {{"flight", ANIM_VALUE_VEC3, 2}, {"skewed", ANIM_VALUE_VEC2, 2}, {"unturned", ANIM_VALUE_VEC3, 1}};

#define CLIP_COUNT ((int)(sizeof CLIPS / sizeof CLIPS[0]))

static void
make_clip_entry(uint8_t* entry, anim_value_t position_type, int track_count) {
    const float times[] = {0.0F, 1.0F};
    const float positions[] = {0, 0, -10, 5, 0, -10};
    const float turns[] = {0, 0, 0, 1, 0, 0, 0, 1};
    test_tracks_header(entry, track_count, 1000);
    test_track_row(entry, 0,
                   &(test_track_t){.path = ANIM_TARGET_SELF,
                                   .field = "position",
                                   .component = ANIM_COMPONENT_TRANSFORM,
                                   .times = FLIGHT_TIMES,
                                   .values = FLIGHT_POSITIONS,
                                   .keys = 2,
                                   .type = position_type,
                                   .interp = ANIM_LINEAR});
    if (track_count > 1) {
        test_track_row(entry, 1,
                       &(test_track_t){.path = ANIM_TARGET_SELF,
                                       .field = "rotation",
                                       .component = ANIM_COMPONENT_TRANSFORM,
                                       .times = FLIGHT_TIMES,
                                       .values = FLIGHT_TURNS,
                                       .keys = 2,
                                       .type = ANIM_VALUE_QUAT,
                                       .interp = ANIM_LINEAR});
    }
    test_pack_put_floats(entry + FLIGHT_TIMES, times, 2);
    test_pack_put_floats(entry + FLIGHT_POSITIONS, positions, 6);
    test_pack_put_floats(entry + FLIGHT_TURNS, turns, 8);
}

#define IDENTITY {{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {0, 0, 0}}
#define AT(x, y, z)                                                                                                    \
    {                                                                                                                  \
        {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, { x, y, z }                                                               \
    }
#define CAMERA_AT(x) AT(x, 0, -CAMERA_DISTANCE) /* a camera's pose, x across */
#define MIRRORED     {{{{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {0, 0, 0}}

/* What a scene entry holds, written out as tools/r3d/scene_asset.py would.
 * Every camera has a half field of view and a near plane of 1. */
typedef struct {
    const char* id;
    int entity_count, renderer_count, camera_count;

    struct {
        const char* name;
        scene_asset_transform_t at;
    } entities[5];

    struct {
        int entity;
        const char* mesh;
    } renderers[2];

    struct {
        int entity;
        uint32_t clear_rgb;
        const char* clip;
    } cameras[2];

    bool skinned, lit;
} scene_spec_t;

typedef struct {
    uint8_t mode;
    uint16_t default_clip;
    vec3f_t ambient;
    float intensity;
    float tonemap_white;
} skin_fixture_options_t;

static const skin_fixture_options_t SKIN_OPTIONS = {R3D_SKIN_LIGHT_DIRECT, 0, {0.25F, 0.5F, 0.75F}, 0.3F, 0.4F};

static const scene_spec_t SCENES[] = {
    /* a camera, a red quad at the origin and a green one 4 units right */
    {"test_pair",
     3,
     2,
     1,
     {{"camera", CAMERA_AT(0)}, {"red", IDENTITY}, {"green", AT(4, 0, 0)}},
     {{1, "red"}, {2, "green"}},
     {{0, CLEAR_RGB, ""}},
     false,
     false},
    /* its own camera and one blue quad at the origin */
    {"test_solo",
     2,
     1,
     1,
     {{"eye", CAMERA_AT(0)}, {"blue", IDENTITY}},
     {{1, "blue"}},
     {{0, CLEAR_RGB, ""}},
     false,
     false},
    /* the same as solo, its camera clearing to a colour of its own */
    {"test_sky",
     2,
     1,
     1,
     {{"eye", CAMERA_AT(0)}, {"blue", IDENTITY}},
     {{1, "blue"}},
     {{0, 0x996633, ""}},
     false,
     false},
    /* names a mesh the pack does not hold */
    {"test_broken", 2, 2, 0, {{"red", IDENTITY}, {"gone", IDENTITY}}, {{0, "red"}, {1, "gone"}}, {{0}}, false, false},
    /* a camera that flies the clip "flight", and a red quad */
    {"test_flight",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "flight"}},
     false,
     false},
    /* two cameras, 4 units apart, and a red quad at the origin */
    {"test_twin",
     3,
     1,
     2,
     {{"left", CAMERA_AT(0)}, {"right", CAMERA_AT(4)}, {"red", IDENTITY}},
     {{2, "red"}},
     {{0, CLEAR_RGB, ""}, {1, CLEAR_RGB, ""}},
     false,
     false},
    {.id = "test_aliases",
     .entity_count = 2,
     .camera_count = 2,
     .entities = {{"camera", CAMERA_AT(0)}, {"right", CAMERA_AT(4)}},
     .cameras = {{.entity = 0, .clear_rgb = CLEAR_RGB, .clip = "flight"},
                 {.entity = 1, .clear_rgb = CLEAR_RGB, .clip = "flight"}}},
    /* a camera flying a clip the pack does not hold */
    {"test_lost",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "nowhere"}},
     false,
     false},
    /* cameras flying a translation of the wrong width, and a clip with no rotation */
    {"test_skewed",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "skewed"}},
     false,
     false},
    {"test_unturned",
     2,
     1,
     1,
     {{"camera", IDENTITY}, {"red", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, "unturned"}},
     false,
     false},

    {.id = "test_lit",
     .lit = true,
     .entity_count = 3,
     .renderer_count = 1,
     .camera_count = 1,
     .entities = {{"camera", CAMERA_AT(0)}, {"red", IDENTITY}, {"sun", IDENTITY}},
     .renderers = {{1, "red"}},
     .cameras = {{0, CLEAR_RGB, ""}}},
    {.id = "test_skin",
     .skinned = true,
     .entity_count = 2,
     .renderer_count = 1,
     .entities = {{"red", IDENTITY}, {"lamp", IDENTITY}},
     .renderers = {{0, "red"}}},
    /* a red quad 2 units left, carrying a green one 4 units to its right */
    {.id = "test_family",
     .entity_count = 3,
     .renderer_count = 2,
     .camera_count = 1,
     .entities = {{"camera", CAMERA_AT(0)}, {"red", AT(-2, 0, 0)}, {"green", AT(4, 0, 0)}}, /* green's parent: red */
     .renderers = {{1, "red"}, {2, "green"}},
     .cameras = {{0, CLEAR_RGB, ""}}},
    /* a scaled root, a turned child, a grandchild that draws; and a second root carrying a quad */
    {.id = "test_tree",
     .entity_count = 5,
     .renderer_count = 2,
     .entities = {{"root", {{{{2, 0, 0}, {0, 2, 0}, {0, 0, 2}}}, {1, 0, 0}}},
                  {"arm", {{{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}}, {0, 2, 0}}},
                  {"hand", AT(1, 0, 0)},
                  {"other", AT(0, 0, 5)},
                  {"cup", AT(0, 1, 0)}},
     .renderers = {{2, "red"}, {4, "green"}}},
    /* a root scaled 2 along x only, carrying a quad */
    {.id = "test_uneven",
     .entity_count = 2,
     .renderer_count = 1,
     .entities = {{"stretched", {{{{2, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {0, 0, 0}}}, {"quad", AT(1, 0, 0)}},
     .renderers = {{1, "red"}}},
    /* a red quad turned inside out */
    {.id = "test_mirror",
     .entity_count = 1,
     .renderer_count = 1,
     .entities = {{"mirror", MIRRORED}},
     .renderers = {{0, "red"}}},
    {"hall",
     2,
     1,
     1,
     {{"camera", CAMERA_AT(0)}, {"statue", IDENTITY}},
     {{1, "red"}},
     {{0, CLEAR_RGB, ""}},
     false,
     false},
    {.id = "meadow",
     .skinned = true,
     .entity_count = 2,
     .renderer_count = 1,
     .entities = {{"subject", IDENTITY}, {"lamp", IDENTITY}},
     .renderers = {{0, "red"}}},
};

/* The bytes of an entity_parents part, padded as the writer pads every part. */
static uint32_t
parents_size(int entities) {
    return (uint32_t)mathi_size_ceil(sizeof(uint16_t) * (size_t)entities, sizeof(uint32_t));
}

enum { FAMILY_PARENT = 1, FAMILY_CHILD = 2 };

/* Which entities have a parent; every other entity is a root. */
static const struct {
    const char* scene;
    int child, parent;
} PARENTS[] = {{"test_family", FAMILY_CHILD, FAMILY_PARENT},
               {"test_tree", 1, 0},
               {"test_tree", 2, 1},
               {"test_tree", 4, 3},
               {"test_uneven", 1, 0}};

static uint16_t
parent_index(const scene_spec_t* s, int entity) {
    for (size_t i = 0; i < sizeof PARENTS / sizeof PARENTS[0]; i++) {
        if (strcmp(s->id, PARENTS[i].scene) == 0 && entity == PARENTS[i].child) {
            return (uint16_t)PARENTS[i].parent;
        }
    }
    return SCENE_ASSET_PARENT_ROOT;
}

#define SCENE_COUNT ((int)(sizeof SCENES / sizeof SCENES[0]))

static const scene_spec_t*
spec_named(const char* id) {
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (strcmp(SCENES[i].id, id) == 0) {
            return &SCENES[i];
        }
    }
    TEST_FAIL_MESSAGE(id);
    return NULL;
}

#define BAD_SCENE "test_bad" /* the pair, its second renderer naming entity 9 */

static int
camera_clip_count(const scene_spec_t* s) {
    int count = 0;
    for (int i = 0; i < s->camera_count; i++) {
        count += s->cameras[i].clip[0] != '\0';
    }
    return count;
}

/* A names row: the name's hash, then the name, NUL padded as the rest of the row is. */
static void
put_name(uint8_t* row, const char* name) {
    test_pack_put32(row + SCENE_ASSET_NAME_AT_HASH, name_hash(name));
    memcpy(row + SCENE_ASSET_NAME_AT_TEXT, name, strlen(name));
}

static uint32_t
scene_entry_size(const scene_spec_t* s) {
    return SCENE_ASSET_HEADER_SIZE
           + ((uint32_t)s->entity_count * (sizeof(scene_asset_name_t) + sizeof(scene_asset_transform_t)))
           + ((uint32_t)s->renderer_count * sizeof(scene_asset_renderer_t))
           + ((uint32_t)s->camera_count * sizeof(scene_asset_camera_t)) + sizeof(scene_asset_settings_t)
           + camera_clip_count(s) * (sizeof(scene_asset_animator_t) + ASSET_NAME_MAX)
           + (s->lit ? sizeof(scene_asset_light_t) : 0)
           + (s->skinned ? sizeof(scene_asset_skinned_renderer_t) - sizeof(scene_asset_renderer_t)
                               + sizeof(scene_asset_animator_t) + 2 * ASSET_NAME_MAX
                         : 0)
           + parents_size(s->entity_count);
}

static void
make_scene_entry_with_skin(uint8_t* entry, const scene_spec_t* s, const skin_fixture_options_t* options) {
    const uint16_t skins = s->skinned;
    const uint16_t clips = camera_clip_count(s) + 2 * skins;
    const uint16_t lights = s->lit;
    const uint16_t counts[SCENE_ASSET_PART_COUNT] = {s->entity_count,
                                                     s->entity_count,
                                                     s->renderer_count - skins,
                                                     s->camera_count,
                                                     lights,
                                                     skins,
                                                     camera_clip_count(s) + skins,
                                                     clips,
                                                     1,
                                                     s->entity_count};
    const size_t sizes[SCENE_ASSET_PART_COUNT] = {
        sizeof(scene_asset_name_t),     sizeof(scene_asset_transform_t),
        sizeof(scene_asset_renderer_t), sizeof(scene_asset_camera_t),
        sizeof(scene_asset_light_t),    sizeof(scene_asset_skinned_renderer_t),
        sizeof(scene_asset_animator_t), ASSET_NAME_MAX,
        sizeof(scene_asset_settings_t), sizeof(uint16_t)};
    uint32_t offsets[SCENE_ASSET_PART_COUNT], at = SCENE_ASSET_HEADER_SIZE;
    test_pack_put16(entry, SCENE_ASSET_VERSION);
    for (int i = 0; i < SCENE_ASSET_PART_COUNT; i++) {
        offsets[i] = at;
        test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + i * sizeof(uint16_t), counts[i]);
        test_pack_put32(entry + SCENE_ASSET_AT_OFFSETS + i * sizeof(uint32_t), at);
        at += counts[i] * sizes[i];
    }
    for (int i = 0; i < s->entity_count; i++) {
        put_name(entry + offsets[SCENE_ASSET_NAMES] + sizeof(scene_asset_name_t) * i, s->entities[i].name);
        memcpy(entry + offsets[SCENE_ASSET_TRANSFORMS] + sizeof(scene_asset_transform_t) * i, &s->entities[i].at,
               sizeof(scene_asset_transform_t));
        test_pack_put16(entry + offsets[SCENE_ASSET_PARENTS] + sizeof(uint16_t) * i, parent_index(s, i));
    }
    for (int i = 0; i < s->renderer_count - skins; i++) {
        const scene_asset_renderer_t row = {.entity = s->renderers[i].entity, .colour_set = SCENE_ASSET_COLOUR_SET_OWN};
        uint8_t* bytes = entry + offsets[SCENE_ASSET_RENDERERS] + sizeof row * i;
        memcpy(bytes, &row, sizeof row);
        memcpy(bytes + offsetof(scene_asset_renderer_t, mesh), s->renderers[i].mesh, strlen(s->renderers[i].mesh));
    }
    if (skins != 0) {
        const scene_asset_skinned_renderer_t skin = {.entity = 0, .mesh = "red", .skin = "rig.quad", .skeleton = "rig"};
        const scene_asset_animator_t animator = {
            .entity = 0, .clip_count = 2, .default_clip = options->default_clip, .wrap = ANIM_CLAMP};
        memcpy(entry + offsets[SCENE_ASSET_SKINS], &skin, sizeof skin);
        memcpy(entry + offsets[SCENE_ASSET_ANIMATORS], &animator, sizeof animator);
        memcpy(entry + offsets[SCENE_ASSET_CLIPS], "rig.move", sizeof "rig.move");
        memcpy(entry + offsets[SCENE_ASSET_CLIPS] + ASSET_NAME_MAX, "rig.back", sizeof "rig.back");
    }
    int clip = 0;
    for (int i = 0; i < s->camera_count; i++) {
        const scene_asset_camera_t row = {.entity = s->cameras[i].entity,
                                          .half_fov_short_tan = 1.0F,
                                          .near_z = 1.0F,
                                          .clear_rgb = s->cameras[i].clear_rgb};
        memcpy(entry + offsets[SCENE_ASSET_CAMERAS] + sizeof row * i, &row, sizeof row);
        if (s->cameras[i].clip[0] != '\0') {
            const scene_asset_animator_t animator = {
                .entity = row.entity, .clip_count = 1, .first_clip = clip, .default_clip = 0, .wrap = ANIM_LOOP};
            memcpy(entry + offsets[SCENE_ASSET_ANIMATORS] + sizeof animator * clip, &animator, sizeof animator);
            memcpy(entry + offsets[SCENE_ASSET_CLIPS] + ASSET_NAME_MAX * clip, s->cameras[i].clip,
                   strlen(s->cameras[i].clip));
            clip++;
        }
    }
    if (lights != 0) {
        const scene_asset_light_t light = {
            .entity = 2, .kind = SCENE_LIGHT_KIND_DIRECTIONAL, .color = {0.25F, 0.5F, 0.75F}, .intensity = 2.0F};
        memcpy(entry + offsets[SCENE_ASSET_LIGHTS], &light, sizeof light);
    }
    const scene_asset_settings_t settings = {.ambient_color = options->ambient,
                                             .ambient_intensity = skins != 0 ? options->intensity : 0.0F,
                                             .tonemap_white =
                                                 skins != 0 ? options->tonemap_white : SCENE_TONEMAP_WHITE_NONE};
    memcpy(entry + offsets[SCENE_ASSET_SETTINGS], &settings, sizeof settings);
}

static void
make_scene_entry(uint8_t* entry, const scene_spec_t* s) {
    make_scene_entry_with_skin(entry, s, &SKIN_OPTIONS);
}

static void
make_rig_skeleton(test_pack_t* pack, const char* id, int joints) {
    const uint32_t rest = ANIM_SKELETON_HEADER_SIZE + joints * ANIM_SKELETON_ROW_SIZE;
    const uint32_t strings = rest + joints * sizeof(float[ANIM_SKELETON_REST_WIDTH]);
    uint8_t* entry = test_pack_add(pack, id, ANIM_SKELETON_ASSET, strings + joints * sizeof("root"));
    test_pack_put16(entry + ANIM_SKELETON_AT_VERSION, ANIM_SKELETON_VERSION);
    entry[ANIM_SKELETON_AT_COUNT] = joints;
    test_pack_put32(entry + ANIM_SKELETON_AT_STRINGS, strings);
    test_pack_put32(entry + ANIM_SKELETON_AT_STRINGS_SIZE, joints * sizeof("root"));
    test_pack_put32(entry + ANIM_SKELETON_AT_REST, rest);
    static const char* const paths[] = {"root", "tip1", "tip2", "tip3"};
    static const float local[ANIM_SKELETON_REST_WIDTH] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1};
    for (int j = 0; j < joints; j++) {
        uint8_t* row = entry + ANIM_SKELETON_HEADER_SIZE + j * ANIM_SKELETON_ROW_SIZE;
        test_pack_put16(row, j * sizeof("root"));
        row[offsetof(anim_skeleton_joint_t, parent)] = j == 0 ? ANIM_SKELETON_ROOT : 0;
        memcpy(entry + strings + j * sizeof("root"), paths[j], sizeof("root"));
        test_pack_put_floats(entry + rest + j * sizeof local, local, ANIM_SKELETON_REST_WIDTH);
    }
}

static void
make_rig_skin(test_pack_t* pack, const char* id, int influences, int joints, int vertices, uint8_t mode) {
    const uint32_t records = R3D_SKIN_HEADER_SIZE + joints * sizeof(float[R3D_SKIN_MATRIX_WIDTH]);
    const int stride = 2 * influences + R3D_SKIN_RECORD_TAIL;
    uint8_t* entry = test_pack_add(pack, id, R3D_SKIN_ASSET, records + vertices * stride);
    test_pack_put16(entry + R3D_SKIN_AT_VERSION, R3D_SKIN_VERSION);
    entry[R3D_SKIN_AT_LIGHTING_MODE] = mode;
    entry[R3D_SKIN_AT_JOINTS] = joints;
    entry[R3D_SKIN_AT_INFLUENCES] = influences;
    test_pack_put32(entry + R3D_SKIN_AT_VERTICES, vertices);
    test_pack_put32(entry + R3D_SKIN_AT_INVERSE, R3D_SKIN_HEADER_SIZE);
    test_pack_put32(entry + R3D_SKIN_AT_RECORDS, records);
    static const float inverse[R3D_SKIN_MATRIX_WIDTH] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int j = 0; j < joints; j++) {
        test_pack_put_floats(entry + R3D_SKIN_HEADER_SIZE + j * sizeof inverse, inverse, R3D_SKIN_MATRIX_WIDTH);
    }
    for (int v = 0; v < vertices; v++) {
        uint8_t* row = entry + records + v * stride;
        for (int i = 0; i < influences; i++) {
            row[i] = i;
            row[influences + i] = R3D_SKIN_WEIGHT_SUM / influences;
        }
        row[influences] += R3D_SKIN_WEIGHT_SUM % influences;
        row[2 * influences + 2] = R3D_SKIN_NORMAL_SCALE;
    }
}

static void
make_rig_clip(test_pack_t* pack, const char* id, const char* target, uint8_t root, int travel) {
    enum {
        KEYS = 2,
        WIDTH = 3,
        TIMES = TEST_TRACKS_HEADER_SIZE + TEST_TRACK_ROW_SIZE + TEST_TRACK_STRING_SLOT,
        VALUES = TIMES + KEYS * sizeof(float),
        BYTES = VALUES + KEYS * WIDTH * sizeof(float)
    };

    uint8_t* entry = test_pack_add(pack, id, ANIM_TRACKS_ASSET, BYTES);
    test_tracks_header(entry, 1, RIG_DURATION_MS);
    entry[ANIM_TRACKS_AT_ROOT] = root;
    const test_track_t track = {target, "position", ANIM_COMPONENT_TRANSFORM, ANIM_VALUE_VEC3, TIMES, VALUES,
                                KEYS,   ANIM_LINEAR};
    test_track_row(entry, 0, &track);
    static const float times[] = {0, 1};
    const float values[] = {0, 0, 0, (float)travel, 0, 0};
    test_pack_put_floats(entry + TIMES, times, KEYS);
    test_pack_put_floats(entry + VALUES, values, KEYS * WIDTH);
}

static void
make_rig_entries(test_pack_t* pack, const skin_fixture_options_t* options) {
    make_rig_skeleton(pack, "rig", RIG_JOINTS);
    make_rig_skeleton(pack, "rig.short", R3D_SKIN_INFLUENCES_MIN);
    make_rig_skin(pack, "rig.skin2", R3D_SKIN_INFLUENCES_MIN, RIG_JOINTS, RIG_VERTICES, R3D_SKIN_LIGHT_DIRECT);
    make_rig_skin(pack, "rig.skin4", R3D_SKIN_INFLUENCES_MAX, RIG_JOINTS, RIG_VERTICES, R3D_SKIN_LIGHT_DIRECT);
    make_rig_skin(pack, "rig.quad", R3D_SKIN_INFLUENCES_MIN, RIG_JOINTS, QUAD_VERTICES, options->mode);
    make_rig_skin(pack, "rig.shortj", R3D_SKIN_INFLUENCES_MIN, R3D_SKIN_INFLUENCES_MIN, RIG_VERTICES,
                  R3D_SKIN_LIGHT_DIRECT);
    make_rig_clip(pack, "rig.move", "root", ANIM_ROOT_SKELETON, RIG_TRAVEL);
    make_rig_clip(pack, "rig.back", "root", ANIM_ROOT_SKELETON, -RIG_TRAVEL);
    make_rig_clip(pack, "property.move", "red", ANIM_ROOT_SCENE, RIG_TRAVEL);
    make_quad_entry(test_pack_add(pack, "rig.mesh", R3D_LIT_MESH_ASSET, RIG_MESH_BYTES), 255, 0, 0, 2);
}

/* The pack of every quad, scene and the clip, with the CRC set. */
static uint32_t
make_pack(uint8_t* bytes, const skin_fixture_options_t* options) {
    test_pack_t pack = test_pack_begin(bytes, PACK_MAX, QUAD_COUNT + SCENE_COUNT + 1 + CLIP_COUNT + RIG_ENTRIES);
    for (int i = 0; i < QUAD_COUNT; i++) {
        uint8_t* entry = test_pack_add(&pack, QUADS[i].name, R3D_LIT_MESH_ASSET, QUAD_BYTES);
        make_quad_entry(entry, QUADS[i].red, QUADS[i].green, QUADS[i].blue, 1);
    }
    for (int i = 0; i < SCENE_COUNT; i++) {
        make_scene_entry_with_skin(test_pack_add(&pack, SCENES[i].id, SCENE_ASSET, scene_entry_size(&SCENES[i])),
                                   &SCENES[i], options);
    }
    uint8_t* bad = test_pack_add(&pack, BAD_SCENE, SCENE_ASSET, scene_entry_size(&SCENES[0]));
    make_scene_entry(bad, &SCENES[0]);
    test_pack_put16(bad + SCENE_ASSET_HEADER_SIZE + (3 * (32 + sizeof(scene_asset_transform_t)))
                        + sizeof(scene_asset_renderer_t),
                    9);
    for (int i = 0; i < CLIP_COUNT; i++) {
        make_clip_entry(test_pack_add(&pack, CLIPS[i].id, ANIM_TRACKS_ASSET, FLIGHT_BYTES), CLIPS[i].position_type,
                        CLIPS[i].track_count);
    }
    make_rig_entries(&pack, options);
    return test_pack_finish(&pack);
}

typedef struct {
    void* raw;      /* what the aligned allocation gave */
    uint8_t* bytes; /* aligned inside it, as a pack must be */
    uint16_t* pixels;
    uint16_t* held; /* a picture kept to compare the next with */
    r3d_instance_t* instances;
    void* saved_skin[2];
    asset_pack_t pack;
    scene_target_t target;
    uint8_t* lent; /* a test's own lender: its block and how much it handed out */
    size_t lent_used;
} fixture_t;

static fixture_t fx;

static void
release_fixture(void) {
    scene_unload_all();
    (void)scene_set_take(NULL);
    memory_free(fx.lent);
#ifndef DEVICE_BUILD
    gfx_reset_for_test();
#endif
    test_free_aligned(fx.raw);
    free(fx.pixels);
    free(fx.held);
    free(fx.instances);
    free(fx.saved_skin[0]);
    free(fx.saved_skin[1]);
    fx = (fixture_t){0};
}

/* Every test starts from an empty engine, so the order of tests is no one's
 * business. */
static void
fixture_with_skin(const skin_fixture_options_t* options) {
    scene_unload_all();
#ifndef DEVICE_BUILD
    hall = NULL;
    statue = SCENE_ENTITY_NONE;
    scene = NULL;
    subject = SCENE_ENTITY_NONE;
#endif
    fx.bytes = test_alloc_aligned(PACK_MAX, ASSET_PACK_BASE_ALIGN, &fx.raw);
    fx.pixels = malloc(sizeof(*fx.pixels) * SIZE * SIZE);
    fx.held = malloc(sizeof(*fx.held) * SIZE * SIZE);
    fx.instances = malloc(sizeof(*fx.instances) * INSTANCES_MAX);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_NOT_NULL(fx.bytes);
    TEST_ASSERT_NOT_NULL(fx.pixels);
    TEST_ASSERT_NOT_NULL(fx.held);
    TEST_ASSERT_NOT_NULL(fx.instances);
    const uint32_t total = make_pack(fx.bytes, options);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&fx.pack, fx.bytes, total));
    fx.target = (scene_target_t){fx.pixels, SIZE, SIZE, NULL, NULL};
}

static void
fixture(void) {
    fixture_with_skin(&SKIN_OPTIONS);
}

static scene_t*
load(const char* name) {
    scene_t* scene = scene_load_from(&fx.pack, name, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, name);
    return scene;
}

static void
assert_light(const scene_t* scene, scene_entity_t entity, vec3f_t color, float intensity) {
    const vec3f_t got = scene_light_color(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(color.x, got.x);
    TEST_ASSERT_EQUAL_FLOAT(color.y, got.y);
    TEST_ASSERT_EQUAL_FLOAT(color.z, got.z);
    TEST_ASSERT_EQUAL_FLOAT(intensity, scene_light_intensity(scene, entity));
}

/* Loads and activates the camera at full size. */
static scene_t*
show(const char* name, name_hash_t camera) {
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

static void
test_a_turned_non_square_scene_places_the_coloured_instance(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);
    fx.target.height = PICTURE_HEIGHT;
    for (int quarter = 1; quarter <= 3; quarter += 2) {
        scene_render(0, quarter, fx.target.width, fx.target.height);
        scene_compose(0, quarter, &fx.target);
        const int offset = quarter == 1 ? GREEN_PIXEL_OFFSET : -GREEN_PIXEL_OFFSET;
        TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), fx.pixels[(((PICTURE_HEIGHT / 2) + offset) * SIZE) + CENTER]);
        TEST_ASSERT_EQUAL_HEX16(RGB565BE(CLEAR_RGB),
                                fx.pixels[((PICTURE_HEIGHT / 2) * SIZE) + CENTER + GREEN_PIXEL_OFFSET]);
    }
}

static void
test_a_scaled_context_preserves_the_non_square_views_destination(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    fx.target.height = PICTURE_HEIGHT;
    scene_render(0, 0, fx.target.width, fx.target.height);
    const transformf_t camera_pose = scene_entity_world(scene, scene->cameras[0].entity);
    const render_view_t view =
        camera_view(scene_camera_lens(scene, SCENE_CAMERA_FIRST), &camera_pose, (viewport_t){SIZE, SIZE / 2, 0});
    render_context_t* c = render_context_main();
    for (int scale = 100; scale >= 50; scale -= 50) {
        render_context_set_scale(c, scale);
        TEST_ASSERT_TRUE(render_context_draw(c, scene->instances, 2, &view, RGB565BE(CLEAR_RGB)));
        TEST_ASSERT_EQUAL_INT(SIZE, c->raster.destination_width);
        TEST_ASSERT_EQUAL_INT(SIZE / 2, c->raster.destination_height);
        TEST_ASSERT_EQUAL_INT(SIZE * scale / 100, c->raster.width);
        TEST_ASSERT_EQUAL_INT(SIZE * scale / 200, c->raster.height);
        render_context_compose(c, fx.pixels, NULL, NULL);
        TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00),
                                fx.pixels[((PICTURE_HEIGHT / 2) * SIZE) + CENTER + GREEN_PIXEL_OFFSET]);
        TEST_ASSERT_EQUAL_HEX16(RGB565BE(CLEAR_RGB),
                                fx.pixels[(((PICTURE_HEIGHT / 2) + GREEN_PIXEL_OFFSET) * SIZE) + CENTER]);
    }
}

static uint16_t
pixel_at(float right_units, float down_units) {
    return fx.pixels[((CENTER + (int)(down_units * PIXELS_PER_UNIT)) * SIZE) + CENTER
                     + (int)(right_units * PIXELS_PER_UNIT)];
}

static uint16_t
pixel(float units_from_center) {
    return pixel_at(units_from_center, 0.0F);
}

static const uint16_t CLEAR = RGB565BE(CLEAR_RGB);

static void
test_a_scene_loads_by_name_and_unloading_gives_back_everything_it_took(void) {
    fixture();
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    scene_t* scene = load("test_pair");
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) < before);
    scene_unload(scene);
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) == before);
}

/* A lender as the app arena is one: a block handed out by bumping. */
#define LENT_BYTES (1024u * 1024u)

static void*
lend(size_t size, size_t align) {
    const size_t at = (fx.lent_used + align - 1) & ~(align - 1);
    if (at > LENT_BYTES || size > LENT_BYTES - at) {
        return NULL;
    }
    fx.lent_used = at + size;
    return fx.lent + at;
}

static bool
lent_from(const void* p) {
    return (const uint8_t*)p >= fx.lent && (const uint8_t*)p < fx.lent + LENT_BYTES;
}

/* With a take installed, the scene and the frame's scratch come from it and
 * the heap is left alone. Unloading frees none of it, and unloading the last
 * scene drops the scratch, so the lender can take everything back. */
static void
test_a_lent_scene_takes_from_its_lender_and_unloading_returns_it_to_the_lender(void) {
    fixture();
    fx.lent = memory_alloc(LENT_BYTES, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(fx.lent);
    (void)scene_set_take(lend);
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    TEST_ASSERT_TRUE(lent_from(scene));
    TEST_ASSERT_TRUE(lent_from(render_context_main()->scratch));
    TEST_ASSERT_EQUAL_size_t(before, memory_free_bytes(MEMORY_PSRAM));
    const size_t taken = fx.lent_used;
    scene_unload(scene);
    TEST_ASSERT_NULL(render_context_main()->scratch);
    TEST_ASSERT_EQUAL_size_t(taken, fx.lent_used);
    TEST_ASSERT_EQUAL_size_t(before, memory_free_bytes(MEMORY_PSRAM));
}

/* A refused take leaves the scene unloaded, or its camera drawing nothing,
 * and takes nothing more. */
static void
test_a_refused_take_loads_nothing_and_a_refused_scratch_draws_nothing(void) {
    fixture();
    fx.lent = memory_alloc(LENT_BYTES, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(fx.lent);
    (void)scene_set_take(lend);
    fx.lent_used = LENT_BYTES;
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_pair", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_MEMORY, why.status);

    fx.lent_used = 0;
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    fx.lent_used = LENT_BYTES;
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
    TEST_ASSERT_NULL(render_context_main()->scratch);
    scene_unload(scene);
}

static void
test_a_camera_clears_to_the_colour_its_scene_gives(void) {
    fixture();
    scene_t* scene = load("test_sky");
    TEST_ASSERT_TRUE(scene_activate(scene, SCENE_CAMERA_FIRST));
    render_context_set_scale(render_context_main(), 100);
    frame(0);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x996633), pixel(-8.0F));
}

/* Dynamic resolution is opt-in: a camera draws at its own scale until a
 * caller asks, again once it asks no more, and again after the app leaves. */
static void
test_a_camera_keeps_its_fixed_scale_unless_dynamic_resolution_is_asked_for(void) {
    fixture();
    scene_t* scene = show("test_sky", SCENE_CAMERA_FIRST);
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
    scene = show("test_sky", SCENE_CAMERA_FIRST);
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
    expect_failure("test_skewed", ASSET_ERR_FORMAT, "skewed");
    expect_failure("red", ASSET_ERR_TYPE, "red"); /* a mesh, not a scene */
    TEST_ASSERT_EQUAL_INT(0, scene_loaded_count());
}

static void
test_a_scene_gives_each_entity_s_mesh_id_and_each_camera_s_lens(void) {
    fixture();
    scene_t* pair = load("test_pair");
    TEST_ASSERT_EQUAL_STRING("green", scene_entity_mesh_id(pair, scene_find(pair, name_hash("green"))));
    TEST_ASSERT_NULL(scene_entity_mesh_id(pair, scene_find(pair, name_hash("camera"))));
    const r3d_lit_mesh_t* green = scene_entity_mesh(pair, scene_find(pair, name_hash("green")));
    TEST_ASSERT_NOT_NULL(green);
    TEST_ASSERT_TRUE(green->vertex_count > 0);
    TEST_ASSERT_NULL(scene_entity_mesh(pair, scene_find(pair, name_hash("camera"))));
    TEST_ASSERT_NOT_NULL(scene_camera_lens(pair, name_hash("camera")));
    TEST_ASSERT_NULL(scene_camera_lens(pair, name_hash("red")));

    scene_t* scene = load("test_flight");
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_EQUAL_UINT32(1000, scene_animator_clip_duration_ms(scene, camera, 0));
    render_context_main()->split.top_offset[0] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, 500));
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MIN - JOB_SHARE_EVEN, render_context_main()->split.top_offset[0]);
    TEST_ASSERT_TRUE(scene_activate(scene, SCENE_CAMERA_FIRST));
    render_context_main()->split.top_offset[0] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, 500));
    TEST_ASSERT_EQUAL_INT(0, render_context_main()->split.top_offset[0]);
    const transformf_t flight = scene_entity_local(scene, camera);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, flight.position.x);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, quatf_rotate(flight.rotation, (vec3f_t){0, 0, 1}).z);
}

/* Where the pair's entry puts its parts, as the pack holds it. */
enum {
    PAIR_NAMES = SCENE_ASSET_HEADER_SIZE,
    PAIR_NAME_TEXT = PAIR_NAMES + SCENE_ASSET_NAME_AT_TEXT,
    PAIR_RENDERERS = PAIR_NAMES + 3 * (sizeof(scene_asset_name_t) + sizeof(scene_asset_transform_t)),
    PAIR_CAMERA = PAIR_RENDERERS + 2 * sizeof(scene_asset_renderer_t),
    PAIR_PARENTS = PAIR_CAMERA + sizeof(scene_asset_camera_t) + sizeof(scene_asset_settings_t),
    PAIR_LAST_ROW_END = PAIR_PARENTS + 3 * sizeof(uint16_t),
    PAIR_BYTES = PAIR_PARENTS + 4 * sizeof(uint16_t), /* three rows, padded to 4 bytes */
    PAIR_HEADER_PAD = SCENE_ASSET_AT_COUNTS + SCENE_ASSET_PART_COUNT * sizeof(uint16_t)
};

/* One edit that breaks the pair's entry: `width` bytes of `value` at `at`,
 * little-endian, or with `fill` every byte of a 32-byte field. */
typedef struct {
    const char* what;
    uint32_t at, width, value;
    bool fill;
    asset_status_t want;
} breakage_t;

static const breakage_t BREAKAGES[] = {
    {"another version", 0, 2, 1, false, ASSET_ERR_VERSION},
    {"names not 4-aligned", SCENE_ASSET_AT_OFFSETS, 4, PAIR_NAMES + 2, false, ASSET_ERR_BOUNDS},
    {"names inside the header", SCENE_ASSET_AT_OFFSETS, 4, 20, false, ASSET_ERR_BOUNDS},
    {"the camera past the end by part of a row", SCENE_ASSET_AT_OFFSETS + SCENE_ASSET_CAMERAS * sizeof(uint32_t), 4,
     PAIR_BYTES, false, ASSET_ERR_BOUNDS},
    {"a name with no NUL", PAIR_NAME_TEXT, 1, 'x', true, ASSET_ERR_FORMAT},
    {"bytes after a name's NUL", PAIR_NAME_TEXT + 20, 1, 'x', false, ASSET_ERR_FORMAT},
    {"an empty entity name", PAIR_NAME_TEXT, 1, 0, true, ASSET_ERR_FORMAT},
    {"a name hash not the name's", PAIR_NAMES + SCENE_ASSET_NAME_AT_HASH, 1, 0x5A, false, ASSET_ERR_FORMAT},
    {"a name changed under its hash", PAIR_NAME_TEXT, 1, 'r', false, ASSET_ERR_FORMAT},
    {"an empty mesh id", PAIR_RENDERERS + 4, 1, 0, true, ASSET_ERR_FORMAT},
    {"a camera of no entity", PAIR_CAMERA, 2, 3, false, ASSET_ERR_FORMAT},
    {"a camera's padding in use", PAIR_CAMERA + 2, 2, 1, false, ASSET_ERR_FORMAT},
    {"a field of view of 0", PAIR_CAMERA + 4, 4, 0, false, ASSET_ERR_FORMAT},
    {"a near plane that is not a number", PAIR_CAMERA + 8, 4, 0x7FC00000U, false, ASSET_ERR_FORMAT},
    {"an infinite field of view", PAIR_CAMERA + 4, 4, 0x7F800000U, false, ASSET_ERR_FORMAT},
    {"a clear colour past 0xFFFFFF", PAIR_CAMERA + 12, 4, 0x1000000U, false, ASSET_ERR_FORMAT},
    {"the header's padding in use", PAIR_HEADER_PAD, 2, 1, false, ASSET_ERR_FORMAT},
    {"an entity its own parent", PAIR_PARENTS + 2, 2, 1, false, ASSET_ERR_FORMAT},
    {"a parent listed after its child", PAIR_PARENTS + 2, 2, 2, false, ASSET_ERR_FORMAT},
    {"a parent past the entities", PAIR_PARENTS + 4, 2, 3, false, ASSET_ERR_FORMAT},
    {"the padding after the parents in use", PAIR_LAST_ROW_END, 2, 1, false, ASSET_ERR_FORMAT},
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

/* NAME_HASH_NONE marks no name, so a name that hashes to it is refused even
 * when the row stores that hash; the bake refuses it first. */
static void
test_a_name_hashing_to_none_is_refused(void) {
    fixture();
    static const char NONE_NAME[] = "akhnp9x";
    TEST_ASSERT_EQUAL_HEX32(NAME_HASH_NONE, name_hash(NONE_NAME));
    const scene_spec_t* pair = spec_named("test_pair");
    const uint32_t size = scene_entry_size(pair);
    void* raw = NULL;
    uint8_t* entry = test_alloc_aligned(size, 4, &raw);
    TEST_ASSERT_NOT_NULL(entry);
    memset(entry, 0, size);
    make_scene_entry(entry, pair);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_entry(entry, size));
    memset(entry + PAIR_NAMES, 0, sizeof(scene_asset_name_t));
    put_name(entry + PAIR_NAMES, NONE_NAME);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(entry, size));
    test_free_aligned(raw);
}

/* Finds compare the stored hashes: each entity is found by its name's hash,
 * and a hash no entity holds, NAME_HASH_NONE among them, finds none. */
static void
test_finds_compare_name_hashes(void) {
    fixture();
    scene_t* pair = load("test_pair");
    for (int i = 0; i < scene_entity_count(pair); i++) {
        TEST_ASSERT_EQUAL_INT(i, scene_find(pair, name_hash(scene_entity_name(pair, (scene_entity_t)i))));
    }
    TEST_ASSERT_EQUAL_INT(SCENE_ENTITY_NONE, scene_find(pair, name_hash("nobody")));
    TEST_ASSERT_EQUAL_INT(SCENE_ENTITY_NONE, scene_find(pair, NAME_HASH_NONE));
    TEST_ASSERT_EQUAL_PTR(scene_camera_lens(pair, SCENE_CAMERA_FIRST), scene_camera_lens(pair, name_hash("camera")));
    TEST_ASSERT_NULL(scene_camera_lens(pair, name_hash("nobody")));
    TEST_ASSERT_FALSE(scene_activate(pair, name_hash("red")));
    TEST_ASSERT_TRUE(scene_activate(pair, name_hash("camera")));
    scene_deactivate();
}

/* Each edit alone is refused with its status; the entry as written opens,
 * and a load of it fails taking nothing. */
static void
test_scene_header_counts_are_independent_of_components(void) {
    fixture();

    enum { ENTITIES = 2, ROWS = ENTITIES + 1 };

    const uint32_t names = SCENE_ASSET_HEADER_SIZE;
    const uint32_t transforms = names + ROWS * sizeof(scene_asset_name_t);
    const uint32_t settings = transforms + ROWS * sizeof(scene_asset_transform_t);
    const uint32_t parents = settings + 2 * sizeof(scene_asset_settings_t);
    const uint32_t size = parents + parents_size(ROWS);
    uint8_t* entry = fx.bytes;
    memset(entry, 0, size);
    test_pack_put16(entry, SCENE_ASSET_VERSION);
    for (int part = 0; part < SCENE_ASSET_PART_COUNT; part++) {
        test_pack_put32(entry + SCENE_ASSET_AT_OFFSETS + part * sizeof(uint32_t), settings);
    }
    test_pack_put32(entry + SCENE_ASSET_AT_OFFSETS + SCENE_ASSET_NAMES * sizeof(uint32_t), names);
    test_pack_put32(entry + SCENE_ASSET_AT_OFFSETS + SCENE_ASSET_TRANSFORMS * sizeof(uint32_t), transforms);
    test_pack_put32(entry + SCENE_ASSET_AT_OFFSETS + SCENE_ASSET_PARENTS * sizeof(uint32_t), parents);
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_PARENTS * sizeof(uint16_t), ENTITIES);
    memset(entry + parents, 0xFF, sizeof(uint16_t) * ROWS); /* every row a root */
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_NAMES * sizeof(uint16_t), ENTITIES);
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_TRANSFORMS * sizeof(uint16_t), ENTITIES);
    test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + SCENE_ASSET_SETTINGS * sizeof(uint16_t), 1);
    for (int i = 0; i < ROWS; i++) {
        char name[ASSET_NAME_MAX];
        (void)snprintf(name, sizeof name, "entity_%d", i);
        put_name(entry + names + i * sizeof(scene_asset_name_t), name);
    }
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_entry(entry, size));

    const struct {
        int part, count;
    } changes[] = {{SCENE_ASSET_TRANSFORMS, ENTITIES - 1},
                   {SCENE_ASSET_TRANSFORMS, ENTITIES + 1},
                   {SCENE_ASSET_PARENTS, ENTITIES - 1},
                   {SCENE_ASSET_PARENTS, ENTITIES + 1},
                   {SCENE_ASSET_SETTINGS, 0},
                   {SCENE_ASSET_SETTINGS, 2}};

    const int entity_parts[] = {SCENE_ASSET_NAMES, SCENE_ASSET_TRANSFORMS, SCENE_ASSET_PARENTS};
    for (size_t i = 0; i < sizeof entity_parts / sizeof entity_parts[0]; i++) {
        test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + entity_parts[i] * sizeof(uint16_t), SCENE_ENTITY_NONE);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_FORMAT, open_entry(entry, size), "as many entities as the none mark");
    for (size_t i = 0; i < sizeof entity_parts / sizeof entity_parts[0]; i++) {
        test_pack_put16(entry + SCENE_ASSET_AT_COUNTS + entity_parts[i] * sizeof(uint16_t), ENTITIES);
    }
    for (size_t i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        const uint32_t at = SCENE_ASSET_AT_COUNTS + changes[i].part * sizeof(uint16_t);
        test_pack_put16(entry + at, changes[i].count);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(entry, size));
        test_pack_put16(entry + at, changes[i].part == SCENE_ASSET_SETTINGS ? 1 : ENTITIES);
    }
}

static void
test_a_child_is_drawn_where_its_parent_places_it(void) {
    fixture();
    scene_t* scene = show("test_family", SCENE_CAMERA_FIRST);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(2.0F));
    transformf_t home = TRANSFORMF_IDENTITY;
    scene_entity_set_local(scene, scene_find(scene, name_hash("red")), home);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(4.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-2.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(2.0F));
}

/* The parent's world is kept, so a child moved alone composes with it. */
static void
test_a_child_moved_alone_stays_in_its_parents_frame(void) {
    fixture();
    scene_t* scene = show("test_family", SCENE_CAMERA_FIRST);
    frame(16);
    transformf_t near = TRANSFORMF_IDENTITY;
    transformf_set_position(&near, (vec3f_t){2, 0, 0});
    scene_entity_set_local(scene, scene_find(scene, name_hash("green")), near);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(2.0F));
}

static void
test_a_disabled_parent_hides_its_children_and_keeps_their_own_flags(void) {
    fixture();
    scene_t* scene = show("test_family", SCENE_CAMERA_FIRST);
    const scene_entity_t red = scene_find(scene, name_hash("red"));
    const scene_entity_t green = scene_find(scene, name_hash("green"));
    scene_entity_set_enabled(scene, red, false);
    TEST_ASSERT_TRUE(scene_entity_enabled(scene, green));
    TEST_ASSERT_FALSE(scene_entity_active(scene, green));
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(2.0F));
    scene_entity_set_enabled(scene, green, false);
    scene_entity_set_enabled(scene, red, true);
    TEST_ASSERT_TRUE(scene_entity_active(scene, red));
    TEST_ASSERT_FALSE(scene_entity_active(scene, green));
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(2.0F));
    scene_entity_set_enabled(scene, green, true);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(2.0F));
}

static void
test_an_entity_knows_its_parent_its_ancestors_and_its_world(void) {
    fixture();
    scene_t* scene = load("test_family");
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    const scene_entity_t red = scene_find(scene, name_hash("red"));
    const scene_entity_t green = scene_find(scene, name_hash("green"));
    TEST_ASSERT_EQUAL_UINT16(SCENE_ENTITY_NONE, scene_entity_parent(scene, red));
    TEST_ASSERT_EQUAL_UINT16(red, scene_entity_parent(scene, green));
    TEST_ASSERT_TRUE(scene_entity_is_within(scene, green, red));
    TEST_ASSERT_TRUE(scene_entity_is_within(scene, red, red));
    TEST_ASSERT_FALSE(scene_entity_is_within(scene, red, green));
    TEST_ASSERT_FALSE(scene_entity_is_within(scene, green, camera));
    TEST_ASSERT_FALSE(scene_entity_is_within(scene, green, SCENE_ENTITY_NONE));
    TEST_ASSERT_EQUAL_FLOAT(2.0F, scene_entity_world(scene, green).position.x);
    TEST_ASSERT_EQUAL_FLOAT(4.0F, scene_entity_local(scene, green).position.x);
}

/* The tree's hand: 1 right of an arm turned a quarter about z and 2 up,
 * under a root scaled 2 and 1 right, so at (1, 6, 0); multiplied the other
 * way round it would be at (1, 3, 0). */
static void
assert_at(vec3f_t want, mat4f_t m) {
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, want.x, m.m[0][3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, want.y, m.m[1][3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, want.z, m.m[2][3]);
}

static mat4f_t
model_of(const scene_t* scene, const char* name) {
    return scene->renderers[scene_renderer_index(scene, scene_find(scene, name_hash(name)))].model;
}

/* Two placements agree in their top three rows: with positive scales, the
 * same position, rotation and scale. */
static void
assert_same_placement(mat4f_t want, mat4f_t got, float tolerance) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_FLOAT_WITHIN(tolerance, want.m[r][c], got.m[r][c]);
        }
    }
}

static void
test_a_world_is_the_parents_world_times_the_entitys_own(void) {
    fixture();
    scene_t* scene = load("test_tree");
    scene_resolve(scene);
    assert_at((vec3f_t){1, 6, 0}, model_of(scene, "hand"));
    assert_at((vec3f_t){1, 6, 0}, scene_entity_world_matrix(scene, scene_find(scene, name_hash("hand"))));
    const transformf_t hand =
        scene_entity_world(scene, scene_find(scene, name_hash("hand"))); /* the root scales evenly */
    assert_same_placement(model_of(scene, "hand"), transformf_compute_matrix(&hand), 1e-5F);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, -2.0F, model_of(scene, "hand").m[0][1]); /* turned, and scaled by 2 */
    assert_at((vec3f_t){0, 1, 5}, model_of(scene, "cup"));
}

/* Under an evenly scaled root and a turned arm, a world set on the hand
 * reads back as set, and is where the hand draws; the stored local is
 * relative to the arm. A leaf may scale unevenly. */
static void
test_a_world_set_under_turned_parents_reads_back_exactly(void) {
    fixture();
    scene_t* scene = load("test_tree");
    const scene_entity_t hand = scene_find(scene, name_hash("hand"));
    transformf_t want = TRANSFORMF_IDENTITY;
    transformf_set_position(&want, (vec3f_t){4, 5, 6});
    transformf_set_rotation(&want, quatf_from_axis_angle((vec3f_t){0, 1, 0}, 0.7F));
    transformf_set_scale(&want, (vec3f_t){1, 2, 3});
    scene_entity_set_world(scene, hand, want);
    const transformf_t got = scene_entity_world(scene, hand);
    assert_same_placement(transformf_compute_matrix(&want), transformf_compute_matrix(&got), 1e-5F);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, 0.5F, scene_entity_local(scene, hand).scale.x); /* under the root's 2 */
    scene_resolve(scene);
    assert_same_placement(transformf_compute_matrix(&got), model_of(scene, "hand"), 1e-4F);
}

/* An entity with children keeps an even scale, so no world below it
 * shears: an entry with an uneven parent does not load, and names it. A
 * leaf may scale unevenly. */
static void
test_an_uneven_scale_on_an_entity_with_children_is_refused(void) {
    fixture();
    scene_t* scene = load("test_tree");
    const scene_entity_t hand = scene_find(scene, name_hash("hand"));
    transformf_t uneven = scene_entity_local(scene, hand);
    transformf_set_scale(&uneven, (vec3f_t){2, 1, 1});
    scene_entity_set_local(scene, hand, uneven);
    TEST_ASSERT_EQUAL_FLOAT(2.0F, scene_entity_local(scene, hand).scale.x);
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_uneven", &why));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
    TEST_ASSERT_EQUAL_STRING("stretched", why.what);
}

/* Moving the root carries its grandchild through the arm; the other root's
 * child stays where it was. Moves made while disabled land on enabling. */
static void
test_a_moved_grandparent_carries_its_grandchild(void) {
    fixture();
    scene_t* scene = load("test_tree");
    scene_resolve(scene);
    const scene_entity_t root = scene_find(scene, name_hash("root"));
    scene_entity_set_enabled(scene, root, false);
    transformf_t moved = scene_entity_local(scene, root);
    transformf_set_position(&moved, (vec3f_t){0, 0, 0});
    scene_entity_set_local(scene, root, moved);
    transformf_set_position(&moved, (vec3f_t){3, 0, 0});
    scene_entity_set_local(scene, root, moved);
    scene_entity_set_enabled(scene, root, true);
    scene_resolve(scene);
    assert_at((vec3f_t){3, 6, 0}, model_of(scene, "hand"));
    assert_at((vec3f_t){0, 1, 5}, model_of(scene, "cup"));
}

static void
test_enabling_carries_down_every_level_and_stops_at_a_disabled_middle(void) {
    fixture();
    scene_t* scene = load("test_tree");
    const scene_entity_t root = scene_find(scene, name_hash("root"));
    const scene_entity_t arm = scene_find(scene, name_hash("arm"));
    const scene_entity_t hand = scene_find(scene, name_hash("hand"));
    const scene_entity_t cup = scene_find(scene, name_hash("cup"));
    scene_entity_set_enabled(scene, root, false);
    TEST_ASSERT_FALSE(scene_entity_active(scene, hand));
    TEST_ASSERT_TRUE(scene_entity_active(scene, cup));
    scene_entity_set_enabled(scene, arm, false);
    scene_entity_set_enabled(scene, root, true);
    TEST_ASSERT_TRUE(scene_entity_active(scene, root));
    TEST_ASSERT_FALSE(scene_entity_active(scene, hand));
    scene_entity_set_enabled(scene, arm, true);
    TEST_ASSERT_TRUE(scene_entity_active(scene, hand));
    TEST_ASSERT_TRUE(scene_entity_is_within(scene, hand, root));
    TEST_ASSERT_FALSE(scene_entity_is_within(scene, cup, root));
}

/* A light's direction and a skin's lit normals are in its own frame, so
 * neither attaches under a parent. */
static void
test_a_light_or_skin_does_not_attach_under_a_parent(void) {
    fixture();
    scene_t* scene = load("test_family");
    const scene_entity_t green = scene_find(scene, name_hash("green"));
    TEST_ASSERT_FALSE(scene_light_set_intensity(scene, green, 1.0F));
    TEST_ASSERT_EQUAL_FLOAT(0.0F, scene_light_intensity(scene, green));
    scene_failure_t why;
    TEST_ASSERT_FALSE(scene_renderer_set_skin(scene, green, "rig.quad", &why));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, scene_find(scene, name_hash("red")), 1.0F));
}

static void
test_a_camera_with_a_parent_is_refused(void) {
    fixture();

    enum {
        FAMILY_CAMERA = SCENE_ASSET_HEADER_SIZE + 3 * (sizeof(scene_asset_name_t) + sizeof(scene_asset_transform_t))
                        + 2 * sizeof(scene_asset_renderer_t)
    };

    const scene_spec_t* family = spec_named("test_family");
    const uint32_t size = scene_entry_size(family);
    void* raw = NULL;
    uint8_t* entry = test_alloc_aligned(size, 4, &raw);
    TEST_ASSERT_NOT_NULL(entry);
    memset(entry, 0, size);
    make_scene_entry(entry, family);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_entry(entry, size));
    test_pack_put16(entry + FAMILY_CAMERA, FAMILY_CHILD);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(entry, size));
    test_free_aligned(raw);
}

/* Until the raster can draw a reflection the right way out, a mirrored
 * transform is refused and named. */
static void
test_a_mirrored_transform_is_refused_by_name(void) {
    fixture();
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_mirror", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
    TEST_ASSERT_EQUAL_STRING("mirror", why.what);
}

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
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_BOUNDS, open_entry(entry, PAIR_LAST_ROW_END - 1), "a byte short");
    memmove(entry + 1, entry, PAIR_BYTES);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_ERR_BOUNDS, open_entry(entry + 1, PAIR_BYTES), "not 4-aligned");
    test_free_aligned(raw);
}

static void
assert_load_failures_release(const asset_pack_t* pack, const char* id) {
    const size_t internal = memory_free_bytes(MEMORY_INTERNAL);
    const size_t psram = memory_free_bytes(MEMORY_PSRAM);

    enum { ALLOCATION_LIMIT = 16 };

    bool succeeded = false;
    for (int allocation = 0; allocation < ALLOCATION_LIMIT; allocation++) {
        scene_fail_allocation_after = allocation;
        scene_failure_t why;
        scene_t* scene = scene_load_from(pack, id, &why);
        scene_fail_allocation_after = -1;
        if (scene != NULL) {
            scene_unload(scene);
            succeeded = true;
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(SCENE_ERR_MEMORY, why.status, why.what);
        }
        TEST_ASSERT_EQUAL_UINT(internal, memory_free_bytes(MEMORY_INTERNAL));
        TEST_ASSERT_EQUAL_UINT(psram, memory_free_bytes(MEMORY_PSRAM));
        if (succeeded) {
            break;
        }
    }
    TEST_ASSERT_TRUE(succeeded);
}

static void
test_a_scene_with_parents_releases_everything_when_a_load_fails(void) {
    fixture();
    assert_load_failures_release(&fx.pack, "test_tree");
}

#ifndef DEVICE_BUILD
static void
test_authored_clip_ids_require_nonempty_terminated_text(void) {
    fixture();
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(getenv("AUTANA_SCENE_PROBE"), &pack, &buffer));
    asset_view_t entry;
    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&pack, "authored", SCENE_ASSET, &entry));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(entry, &asset));
    uint8_t* bytes = (uint8_t*)(uintptr_t)entry.data;
    const uint32_t size = entry.size;
    for (int i = 0; i < asset.clip_count; i++) {
        char* clip = (char*)(uintptr_t)asset.clips[i];
        char saved[ASSET_NAME_MAX];
        memcpy(saved, clip, sizeof saved);
        memset(clip, 'x', sizeof saved);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(bytes, size));
        memset(clip, 0, sizeof saved);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(bytes, size));
        memcpy(clip, saved, sizeof saved);
        TEST_ASSERT_EQUAL_INT(ASSET_OK, open_entry(bytes, size));
    }
    asset_file_release(buffer);
}

static void
check_authored_colors(uint8_t* bytes, uint32_t size, uint32_t light, uint32_t settings) {
    const float invalid[] = {-1.0F, INFINITY, NAN};
    const uint32_t fields[] = {light + offsetof(scene_asset_light_t, color),
                               light + offsetof(scene_asset_light_t, intensity),
                               settings + offsetof(scene_asset_settings_t, ambient_color),
                               settings + offsetof(scene_asset_settings_t, ambient_intensity),
                               settings + offsetof(scene_asset_settings_t, tonemap_white)};
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
        float saved;
        memcpy(&saved, bytes + fields[f], sizeof saved);
        for (size_t v = 0; v < sizeof invalid / sizeof invalid[0]; v++) {
            memcpy(bytes + fields[f], &invalid[v], sizeof invalid[v]);
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(bytes, size));
        }
        memcpy(bytes + fields[f], &saved, sizeof saved);
    }
    const uint32_t colors[] = {light + offsetof(scene_asset_light_t, color), settings};
    for (size_t c = 0; c < sizeof colors / sizeof colors[0]; c++) {
        for (int axis = 0; axis < R3D_SKIN_LIGHT_CHANNELS; axis++) {
            const uint32_t at = colors[c] + axis * sizeof(float);
            float saved;
            const float too_bright = 1.1F;
            memcpy(&saved, bytes + at, sizeof saved);
            memcpy(bytes + at, &too_bright, sizeof too_bright);
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(bytes, size));
            memcpy(bytes + at, &saved, sizeof saved);
        }
    }
}

static void
test_authored_rows_and_refusals(void) {
    fixture();
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(getenv("AUTANA_SCENE_PROBE"), &pack, &buffer));
    asset_view_t entry;
    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&pack, "authored", SCENE_ASSET, &entry));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(entry, &asset));
    TEST_ASSERT_EQUAL_INT(2, asset.light_count);
    TEST_ASSERT_EQUAL_INT(2, asset.skinned_renderer_count);
    TEST_ASSERT_EQUAL_INT(2, asset.animator_count);
    TEST_ASSERT_EQUAL_FLOAT(1.5F, asset.lights[0].intensity);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, asset.settings->tonemap_white);
    TEST_ASSERT_EQUAL_STRING("authored.first.skin", asset.skinned_renderers[0].skin);
    TEST_ASSERT_EQUAL_STRING("walk", asset.clips[1]);
    TEST_ASSERT_EQUAL_STRING("first", asset.names[1].name);
    TEST_ASSERT_EQUAL_HEX32(name_hash("first"), asset.names[1].hash); /* the tools' hash is the device's */
    uint8_t* bytes = (uint8_t*)(uintptr_t)entry.data;
    const uint32_t light = (const uint8_t*)asset.lights - entry.data;
    const uint32_t skin = (const uint8_t*)asset.skinned_renderers - entry.data;
    const uint32_t animator = (const uint8_t*)asset.animators - entry.data;
    const uint32_t settings = (const uint8_t*)asset.settings - entry.data;
    const breakage_t changes[] = {
        {"light entity", light, 2, asset.entity_count, false, ASSET_ERR_FORMAT},
        {"skin entity", skin, 2, asset.entity_count, false, ASSET_ERR_FORMAT},
        {"animator entity", animator, 2, asset.entity_count, false, ASSET_ERR_FORMAT},
        {"fewer transforms", SCENE_ASSET_AT_COUNTS + SCENE_ASSET_TRANSFORMS * sizeof(uint16_t), 2,
         asset.entity_count - 1, false, ASSET_ERR_FORMAT},
        {"more transforms", SCENE_ASSET_AT_COUNTS + SCENE_ASSET_TRANSFORMS * sizeof(uint16_t), 2,
         asset.entity_count + 1, false, ASSET_ERR_FORMAT},
        {"no settings", SCENE_ASSET_AT_COUNTS + SCENE_ASSET_SETTINGS * sizeof(uint16_t), 2, 0, false, ASSET_ERR_FORMAT},
        {"two settings", SCENE_ASSET_AT_COUNTS + SCENE_ASSET_SETTINGS * sizeof(uint16_t), 2, 2, false,
         ASSET_ERR_FORMAT},
        {"light kind zero", light + offsetof(scene_asset_light_t, kind), 1, 0, false, ASSET_ERR_FORMAT},
        {"light kind next", light + offsetof(scene_asset_light_t, kind), 1, SCENE_LIGHT_KIND_DIRECTIONAL + 1, false,
         ASSET_ERR_FORMAT},
        {"wrap next", animator + offsetof(scene_asset_animator_t, wrap), 1, ANIM_WRAP_LOOP + 1, false,
         ASSET_ERR_FORMAT},
        {"overflow clips", animator + offsetof(scene_asset_animator_t, first_clip), 4, UINT32_MAX, false,
         ASSET_ERR_FORMAT},
        {"light pad", light + offsetof(scene_asset_light_t, pad), 1, 1, false, ASSET_ERR_FORMAT},
        {"two lights", light + sizeof(scene_asset_light_t), 2, asset.lights[0].entity, false, ASSET_ERR_FORMAT},
        {"skin pad", skin + offsetof(scene_asset_skinned_renderer_t, pad), 2, 1, false, ASSET_ERR_FORMAT},
        {"two skins", skin + sizeof(scene_asset_skinned_renderer_t), 2, asset.skinned_renderers[0].entity, false,
         ASSET_ERR_FORMAT},
        {"two animators", animator + sizeof(scene_asset_animator_t), 2, asset.animators[0].entity, false,
         ASSET_ERR_FORMAT},
        {"default clip", animator + offsetof(scene_asset_animator_t, default_clip), 2, 1, false, ASSET_ERR_FORMAT},
        {"animator pad", animator + offsetof(scene_asset_animator_t, pad), 1, 1, false, ASSET_ERR_FORMAT},
    };
    for (size_t i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        uint32_t saved;
        memcpy(&saved, bytes + changes[i].at, changes[i].width);
        apply(bytes, &changes[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(changes[i].want, open_entry(bytes, entry.size), changes[i].what);
        memcpy(bytes + changes[i].at, &saved, changes[i].width);
    }
    const uint32_t entities[] = {skin, animator};
    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++) {
        uint16_t saved;
        memcpy(&saved, bytes + entities[i], sizeof saved);
        test_pack_put16(bytes + entities[i], asset.entity_count - 1);
        TEST_ASSERT_EQUAL_INT(ASSET_OK, open_entry(bytes, entry.size));
        test_pack_put16(bytes + entities[i], saved);
    }
    const uint32_t ids[] = {skin + offsetof(scene_asset_skinned_renderer_t, mesh),
                            skin + offsetof(scene_asset_skinned_renderer_t, skin),
                            skin + offsetof(scene_asset_skinned_renderer_t, skeleton)};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        char saved[ASSET_NAME_MAX];
        memcpy(saved, bytes + ids[i], sizeof saved);
        memset(bytes + ids[i], 'x', sizeof saved);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(bytes, entry.size));
        memset(bytes + ids[i], 0, sizeof saved);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_entry(bytes, entry.size));
        memcpy(bytes + ids[i], saved, sizeof saved);
    }
    check_authored_colors(bytes, entry.size, light, settings);
    test_pack_put16(bytes + animator + offsetof(scene_asset_animator_t, default_clip), SCENE_ASSET_DEFAULT_CLIP_NONE);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, open_entry(bytes, entry.size));
    asset_file_release(buffer);
}

static void
test_encoder_authored_components_load_and_reach_runtime(void) {
    fixture();
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(getenv("AUTANA_SCENE_PROBE"), &pack, &buffer));
    scene_failure_t why;
    scene_t* loaded = scene_load_from(&pack, "authored", &why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SCENE_OK, why.status, why.what);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_INT(2, loaded->light_count);
    const scene_entity_t entity = scene_find(loaded, name_hash("first"));
    TEST_ASSERT_EQUAL_INT(1, entity);
    TEST_ASSERT_EQUAL_STRING("authored.first", scene_entity_mesh_id(loaded, entity));
    TEST_ASSERT_NOT_NULL(loaded->skinned_renderers[entity].internal_storage);
    TEST_ASSERT_NOT_NULL(loaded->light_transfer);
    assert_light(loaded, scene_find(loaded, name_hash("sun")), (vec3f_t){0.8F, 0.7F, 0.6F}, 1.5F);
    scene_unload(loaded);
    asset_file_release(buffer);
}

static void
test_authored_animator_clip_array_allocation_failure_releases_memory(void) {
    fixture();
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(getenv("AUTANA_SCENE_PROBE"), &pack, &buffer));
    const size_t internal = memory_free_bytes(MEMORY_INTERNAL);
    const size_t psram = memory_free_bytes(MEMORY_PSRAM);

    enum { ALLOCATION_SWEEP_LIMIT = 64 };

    bool completed = false;
    for (int allocation = 0; allocation < ALLOCATION_SWEEP_LIMIT; allocation++) {
        scene_fail_allocation_after = allocation;
        scene_failure_t why;
        scene_t* loaded = scene_load_from(&pack, "authored", &why);
        scene_fail_allocation_after = -1;
        if (loaded != NULL) {
            TEST_ASSERT_GREATER_THAN_INT(0, allocation);
            TEST_ASSERT_EQUAL_INT(SCENE_OK, why.status);
            scene_unload(loaded);
            completed = true;
        } else {
            TEST_ASSERT_EQUAL_INT(SCENE_ERR_MEMORY, why.status);
        }
        TEST_ASSERT_EQUAL_UINT(internal, memory_free_bytes(MEMORY_INTERNAL));
        TEST_ASSERT_EQUAL_UINT(psram, memory_free_bytes(MEMORY_PSRAM));
        if (completed) {
            break;
        }
    }
    asset_file_release(buffer);
    TEST_ASSERT_TRUE(completed);
}

static void
test_encoder_authored_two_skin_allocation_failures_release_lighting(void) {
    fixture();
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(getenv("AUTANA_SCENE_PROBE"), &pack, &buffer));
    assert_load_failures_release(&pack, "authored");
    asset_file_release(buffer);
}

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
    make_quad_entry(test_pack_add(&built, "red", R3D_LIT_MESH_ASSET, QUAD_BYTES), 255, 0, 0, 1);
    make_quad_entry(test_pack_add(&built, "green", R3D_LIT_MESH_ASSET, QUAD_BYTES), 0, 255, 0, 1);
    memcpy(test_pack_add(&built, "probe_scene", SCENE_ASSET, scene_entry.size), scene_entry.data, scene_entry.size);
    memcpy(test_pack_add(&built, "probe", ANIM_TRACKS_ASSET, clip_entry.size), clip_entry.data, clip_entry.size);
    asset_file_release(buffer);
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, fx.bytes, test_pack_finish(&built)));

    scene_t* scene = scene_load_from(&pack, "probe_scene", NULL);
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_EQUAL_UINT16(0, scene_find(scene, name_hash("red")));
    TEST_ASSERT_EQUAL_UINT16(1, scene_find(scene, name_hash("camera")));
    TEST_ASSERT_EQUAL_UINT16(2, scene_find(scene, name_hash("green")));
    TEST_ASSERT_EQUAL_STRING("red", scene_entity_mesh_id(scene, 0));
    TEST_ASSERT_EQUAL_STRING("green", scene_entity_mesh_id(scene, 2));
    TEST_ASSERT_NULL(scene_entity_mesh_id(scene, 1));
    /* a quarter turn about y, scaled (2, 4, 0.5), at (1, 2, 3) */
    const mat4f_t want = mat4f_from_trs((vec3f_t){1, 2, 3}, quatf_from_axis_angle((vec3f_t){0, 1, 0}, MATH_PI * 0.5F),
                                        (vec3f_t){2, 4, 0.5F});
    const transformf_t pose = scene_entity_local(scene, 0);
    const mat4f_t got = transformf_compute_matrix(&pose);
    for (int row = 0; row < MAT3F_AXES; row++) {
        for (int column = 0; column < MAT3F_AXES; column++) {
            TEST_ASSERT_FLOAT_WITHIN(1e-6F, want.m[row][column], got.m[row][column]);
        }
        TEST_ASSERT_EQUAL_FLOAT(want.m[row][MAT3F_AXES], got.m[row][MAT3F_AXES]);
    }
    const camera_t* lens = scene_camera_lens(scene, name_hash("camera"));
    TEST_ASSERT_EQUAL_FLOAT(0.75F, lens->half_fov_short_tan);
    TEST_ASSERT_EQUAL_FLOAT(1.5F, lens->near_z);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(CLEAR_RGB), scene->cameras[0].clear);
    TEST_ASSERT_EQUAL_UINT32(3000, scene_animator_clip_duration_ms(scene, scene_find(scene, name_hash("camera")),
                                                                   0)); /* the probe clip's length */
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

static void
test_doc_scene_load_find_activate(void) {
    fixture();
    test_asset_dir_use(".");
    write_pack("hall", false);
    hall_example_start();
    remove_pack("hall");
    TEST_ASSERT_NOT_NULL(hall);
    TEST_ASSERT_EQUAL_INT(scene_find(hall, name_hash("statue")), statue);
    TEST_ASSERT_EQUAL_INT(SCENE_ENTITY_NONE, scene_find(hall, name_hash("missing")));
    TEST_ASSERT_TRUE(scene_has_active_camera());
    frame(0);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), fx.pixels[CENTER * SIZE + CENTER]);
}

static void
test_doc_scene_missing_leaves_entities_and_camera_alone(void) {
    fixture();
    test_asset_dir_use(".");
    statue = 0;
    hall_example_start();
    TEST_ASSERT_NULL(hall);
    TEST_ASSERT_EQUAL_INT(0, statue);
    TEST_ASSERT_FALSE(scene_has_active_camera());
    scene_failure_t why = {0};
    TEST_ASSERT_NULL(scene_load("hall", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, why.asset);
    TEST_ASSERT_EQUAL_STRING("hall", why.what);
}

static void
test_doc_animator_blends_to_selected_clip(void) {
    fixture();
    test_asset_dir_use(".");
    write_pack("meadow", false);
    start();
    remove_pack("meadow");
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_EQUAL_INT(scene_find(scene, name_hash("subject")), subject);
    TEST_ASSERT_TRUE(on_pick(1));
    const anim_layer_t* layer = &scene->animators[subject].layer;
    TEST_ASSERT_EQUAL_INT(1, layer->current.clip);
    TEST_ASSERT_EQUAL_INT(0, layer->previous.clip);
    TEST_ASSERT_EQUAL_UINT32(BLEND_MS, layer->blend_ms);
    TEST_ASSERT_FALSE(on_pick(scene_animator_clip_count(scene, subject)));
}

static void
test_doc_present_modes_reach_camera(void) {
    fixture();
    scene_t* loaded = show("test_pair", SCENE_CAMERA_FIRST);
    const scene_entity_t camera = scene_find(loaded, name_hash("camera"));
    TEST_ASSERT_TRUE(present_example(loaded, camera));
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_SCANLINE, scene_camera_lens(loaded, SCENE_CAMERA_FIRST)->present);
    TEST_ASSERT_FALSE(present_example(loaded, scene_find(loaded, name_hash("red"))));
}

static void
test_doc_small_fill_reaches_camera(void) {
    fixture();
    scene_t* loaded = show("test_pair", SCENE_CAMERA_FIRST);
    const small_fill_t before = scene_camera_lens(loaded, SCENE_CAMERA_FIRST)->small_fill;
    const small_fill_t expected = SMALL_FILL_FAST;
    TEST_ASSERT_TRUE(before.ceiling != expected.ceiling || before.gate != expected.gate
                     || before.style != expected.style);
    TEST_ASSERT_TRUE(small_fill_example(loaded, scene_find(loaded, name_hash("camera"))));
    const small_fill_t actual = scene_camera_lens(loaded, SCENE_CAMERA_FIRST)->small_fill;
    TEST_ASSERT_EQUAL_INT(expected.ceiling, actual.ceiling);
    TEST_ASSERT_EQUAL_INT(expected.gate, actual.gate);
    TEST_ASSERT_EQUAL_INT(expected.style, actual.style);
    TEST_ASSERT_FALSE(small_fill_example(loaded, scene_find(loaded, name_hash("red"))));
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
    scene_fail_allocation_after = 0;
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
    TEST_ASSERT_EQUAL_UINT16(1, scene_find(scene, name_hash("red")));
    TEST_ASSERT_EQUAL_UINT16(2, scene_find(scene, name_hash("green")));
    TEST_ASSERT_EQUAL_UINT16(SCENE_ENTITY_NONE, scene_find(scene, name_hash("yellow")));
    TEST_ASSERT_EQUAL_FLOAT(4.0F, scene_entity_local(scene, scene_find(scene, name_hash("green"))).position.x);
}

enum { LCOL_TEST_SETS = 2, MESH_COLOURS_WORD = 6 };

static void
test_lcol_shared_names(void) {
    fixture();
    static const char* const meshes[] = LCOL_TEST_MESHES;
    static const char* const names[] = LCOL_TEST_NAMES;
    static const char* const invalid[] = LCOL_TEST_INVALID;
    for (size_t i = 0; i < sizeof meshes / sizeof meshes[0]; i++) {
        test_pack_t pack = test_pack_begin(fx.bytes, PACK_MAX, 1);
        uint8_t* colors = test_pack_add(&pack, names[i], LCOL_ASSET, LCOL_HEADER_SIZE + LCOL_RGB888_BYTES);
        test_pack_put16(colors + LCOL_AT_VERSION, LCOL_VERSION);
        test_pack_put16(colors + LCOL_AT_ENCODING, LCOL_ENCODING_RGB888);
        test_pack_put32(colors + LCOL_AT_SET_COUNT, 1);
        test_pack_put32(colors + LCOL_AT_COUNT, 1);
        const uint32_t total = test_pack_finish(&pack);
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&fx.pack, fx.bytes, total));
        r3d_lit_mesh_t mesh = {.vertex_count = 1};
        TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open_colours(&fx.pack, meshes[i], 0, &mesh));
        TEST_ASSERT_EQUAL_PTR(colors + LCOL_HEADER_SIZE, mesh.colors);
    }
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        r3d_lit_mesh_t mesh = {0};
        const asset_status_t expected = invalid[i][0] == '\0' ? ASSET_ERR_FORMAT : ASSET_ERR_BOUNDS;
        TEST_ASSERT_EQUAL_INT(expected, r3d_lit_mesh_open_colours(&fx.pack, invalid[i], 0, &mesh));
    }
}

static void
test_skinned_mesh_requires_colours(void) {
    const skin_fixture_options_t options = {.mode = R3D_SKIN_LIGHT_DIRECT};
    fixture_with_skin(&options);
    scene_t* scene = load("test_skin");
    TEST_ASSERT_NOT_NULL(scene->renderers[0].mesh.colors);
    scene_unload_all();
    asset_view_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&fx.pack, "red", R3D_LIT_MESH_ASSET, &mesh));

    test_pack_put32((uint8_t*)mesh.data + MESH_COLOURS_WORD * sizeof(uint32_t), 0);
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_skin", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
}

typedef struct {
    const char* id;
    asset_status_t expected;
    uint16_t first_set, second_set;
    bool missing, coloured, wrong_encoding, short_entry;
    uint16_t version_delta;
    uint32_t count_extra, payload_extra;
} lcol_case_t;

static void
make_lcol_scene(test_pack_t* pack, const lcol_case_t* spec, const char* message) {
    const uint32_t size = scene_entry_size(&SCENES[0]);
    uint8_t* entry = test_pack_add(pack, "test_pair", SCENE_ASSET, size);
    make_scene_entry(entry, &SCENES[0]);
    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, scene_asset_open((asset_view_t){entry, size}, &asset), message);
    scene_asset_renderer_t* rows = (scene_asset_renderer_t*)(void*)asset.renderers;
    test_pack_put16((uint8_t*)&rows[0] + sizeof(uint16_t), spec->first_set);
    test_pack_put16((uint8_t*)&rows[1] + sizeof(uint16_t), spec->second_set);
    memset(rows[1].mesh, 0, sizeof rows[1].mesh);
    memcpy(rows[1].mesh, "red", sizeof "red");
}

static void
make_lcol_entry(test_pack_t* pack, const lcol_case_t* spec, uint16_t encoding) {
    const int count = encoding == LCOL_ENCODING_RGB888 ? QUAD_VERTICES : QUAD_TRIANGLES;
    const int stride = encoding == LCOL_ENCODING_RGB888 ? LCOL_RGB888_BYTES : LCOL_RGB565_BYTES;
    uint8_t* colors = test_pack_add(pack, "red.colors", LCOL_ASSET,
                                    LCOL_HEADER_SIZE + LCOL_TEST_SETS * count * stride + spec->payload_extra);
    test_pack_put16(colors + LCOL_AT_VERSION, LCOL_VERSION + spec->version_delta);
    test_pack_put16(colors + LCOL_AT_ENCODING, spec->wrong_encoding ? 1 - encoding : encoding);
    test_pack_put32(colors + LCOL_AT_SET_COUNT, LCOL_TEST_SETS);
    test_pack_put32(colors + LCOL_AT_COUNT, count + spec->count_extra);
    for (int v = 0; v < count; v++) {
        if (encoding == LCOL_ENCODING_RGB888) {
            colors[LCOL_HEADER_SIZE + v * stride] = UINT8_MAX;
            colors[LCOL_HEADER_SIZE + (count + v) * stride + 1] = UINT8_MAX;
        } else {
            test_pack_put16(colors + LCOL_HEADER_SIZE + v * stride, RGB565BE(0xFF0000));
            test_pack_put16(colors + LCOL_HEADER_SIZE + (count + v) * stride, RGB565BE(0x00FF00));
        }
    }
}

static void
make_lcol_pack(const lcol_case_t* spec, uint16_t encoding, const char* message) {
    enum { ENTRY_SIZE_AT = ASSET_NAME_MAX + 2 * sizeof(uint32_t) };

    test_pack_t pack = test_pack_begin(fx.bytes, PACK_MAX, spec->missing ? 2 : 3);
    uint8_t* mesh = test_pack_add(&pack, "red", R3D_LIT_MESH_ASSET, QUAD_BYTES);
    make_quad_entry(mesh, UINT8_MAX, 0, 0, 1);
    if (!spec->coloured) {
        test_pack_put32(mesh + MESH_COLOURS_WORD * sizeof(uint32_t), 0);
    }
    make_lcol_scene(&pack, spec, message);
    if (!spec->missing) {
        make_lcol_entry(&pack, spec, encoding);
    }
    if (spec->short_entry) {
        test_pack_put32(pack.bytes + ASSET_PACK_HEADER_SIZE + (pack.added - 1) * ASSET_PACK_ENTRY_SIZE + ENTRY_SIZE_AT,
                        LCOL_HEADER_SIZE - 1);
    }
    const uint32_t total = test_pack_finish(&pack);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, asset_pack_open(&fx.pack, fx.bytes, total), message);
}

static void
assert_lcol_scene(const lcol_case_t* spec, const char* message) {
    scene_failure_t why;
    scene_t* scene = scene_load_from(&fx.pack, "test_pair", &why);
    if (spec->expected != ASSET_OK) {
        TEST_ASSERT_NULL_MESSAGE(scene, message);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SCENE_ERR_ASSET, why.status, message);
        TEST_ASSERT_EQUAL_INT_MESSAGE(spec->expected, why.asset, message);
        return;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(scene, message);
    TEST_ASSERT_TRUE_MESSAGE(scene_activate(scene, SCENE_CAMERA_FIRST), message);
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(RGB565BE(0xFF0000), pixel(0.0F), message);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(RGB565BE(0x00FF00), pixel(4.0F), message);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(scene->renderers[0].mesh.positions, scene->renderers[1].mesh.positions, message);
}

static void
test_lcol_instances_and_refusals(void) {
    static const lcol_case_t cases[] = {
        {.id = "valid", .expected = ASSET_OK, .second_set = LCOL_TEST_SETS - 1},
        {.id = "range", .expected = ASSET_ERR_FORMAT, .second_set = LCOL_TEST_SETS},
        {.id = "missing", .expected = ASSET_ERR_NOT_FOUND, .missing = true},
        {.id = "count", .expected = ASSET_ERR_FORMAT, .count_extra = 1},
        {.id = "encoding", .expected = ASSET_ERR_FORMAT, .wrong_encoding = true},
        {.id = "colourless-own",
         .expected = ASSET_ERR_FORMAT,
         .first_set = SCENE_ASSET_COLOUR_SET_OWN,
         .missing = true},
        {.id = "version", .expected = ASSET_ERR_VERSION, .version_delta = 1},
        {.id = "short", .expected = ASSET_ERR_BOUNDS, .short_entry = true},
        {.id = "oversized", .expected = ASSET_ERR_BOUNDS, .payload_extra = sizeof(uint16_t)},
        {.id = "coloured-prototype",
         .expected = ASSET_ERR_FORMAT,
         .second_set = SCENE_ASSET_COLOUR_SET_OWN,
         .coloured = true},
    };
    fixture();
    for (uint16_t encoding = LCOL_ENCODING_RGB888; encoding <= LCOL_ENCODING_RGB565; encoding++) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            char message[64];
            snprintf(message, sizeof message, "LCOL case %s encoding %u", cases[i].id, encoding);
            scene_unload_all();
            make_lcol_pack(&cases[i], encoding, message);
            assert_lcol_scene(&cases[i], message);
        }
    }
}

static void
test_the_active_camera_draws_its_scenes_renderers(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(4.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-4.0F));
}

static void
test_a_disabled_entity_is_not_drawn_and_enabling_it_draws_it_again(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("green")), false);
    TEST_ASSERT_FALSE(scene_entity_enabled(scene, scene_find(scene, name_hash("green"))));
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(4.0F));
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("green")), true);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(4.0F));
}

/* A mesh handed in at run time is drawn in the loaded one's place, a view
 * with no nodes draws nothing, and an entity with no renderer refuses. */
static void
test_a_runtime_mesh_is_drawn_in_place_of_the_loaded_one(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    const scene_entity_t green = scene_find(scene, name_hash("green"));
    r3d_lit_mesh_t blue = *scene_entity_mesh(scene, green);
    uint8_t (*colors)[3] = malloc(sizeof(uint8_t[3]) * (size_t)blue.vertex_count);
    TEST_ASSERT_NOT_NULL(colors);
    for (int v = 0; v < blue.vertex_count; v++) {
        colors[v][0] = 0;
        colors[v][1] = 0;
        colors[v][2] = 0xFF;
    }
    blue.colors = (const uint8_t (*)[3])colors;
    TEST_ASSERT_TRUE(scene_renderer_set_mesh(scene, green, &blue));
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x0000FF), pixel(4.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    blue.node_count = 0;
    TEST_ASSERT_TRUE(scene_renderer_set_mesh(scene, green, &blue));
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(4.0F));
    TEST_ASSERT_FALSE(scene_renderer_set_mesh(scene, scene_find(scene, name_hash("camera")), &blue));
    free(colors);
}

/* A camera takes a lens shift; anything else, or a shift that is not a
 * number, is refused. */
static void
test_a_camera_takes_a_lens_shift_and_nothing_else_does(void) {
    fixture();
    scene_t* scene = load("test_pair");
    const vec2f_t shift = {0.25F, -0.1F};
    TEST_ASSERT_TRUE(scene_camera_set_shift(scene, scene_find(scene, name_hash("camera")), shift));
    TEST_ASSERT_FALSE(scene_camera_set_shift(scene, scene_find(scene, name_hash("red")), shift));
    TEST_ASSERT_FALSE(scene_camera_set_shift(scene, scene_find(scene, name_hash("camera")), (vec2f_t){NAN, 0.0F}));
}

static void
test_a_moved_entity_is_drawn_where_it_now_stands(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    transformf_t away = TRANSFORMF_IDENTITY;
    transformf_set_position(&away, (vec3f_t){-4, 0, 0});
    scene_entity_set_local(scene, scene_find(scene, name_hash("red")), away);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-4.0F));
    const transformf_t home = TRANSFORMF_IDENTITY;
    scene_entity_set_local(scene, scene_find(scene, name_hash("red")), home);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
}

/* The green quad stands off the view axis, so a roll of the camera about it
 * moves the quad round the picture: a quarter turn takes it from beside the
 * centre to above or below it. */
static void
test_a_camera_rolled_about_its_view_axis_draws_the_picture_turned(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    const uint16_t green = RGB565BE(0x00FF00);
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    const float beside = scene_entity_local(scene, scene_find(scene, name_hash("green"))).position.x;
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(green, pixel(beside));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel_at(0.0F, -beside));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel_at(0.0F, beside));

    transformf_t rolled = scene_entity_local(scene, camera);
    transformf_set_rotation(&rolled,
                            quatf_mul(rolled.rotation, quatf_from_axis_angle((vec3f_t){0, 0, 1}, MATH_PI / 2)));
    scene_entity_set_local(scene, camera, rolled);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(beside));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-beside));
    const bool above = pixel_at(0.0F, -beside) == green;
    const bool below = pixel_at(0.0F, beside) == green;
    TEST_ASSERT_TRUE(above != below);
}

static void
test_with_nothing_enabled_the_picture_is_left_alone(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("red")), false);
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("green")), false);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
}

static void
test_two_scenes_are_held_at_once_and_the_active_camera_decides_which_is_seen(void) {
    fixture();
    scene_t* pair = show("test_pair", SCENE_CAMERA_FIRST);
    scene_t* solo = load("test_solo");
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    render_context_main()->split.top_offset[0] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    TEST_ASSERT_TRUE(scene_activate(solo, name_hash("eye")));
    TEST_ASSERT_EQUAL_INT(0, render_context_main()->split.top_offset[0]);
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x0000FF), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(4.0F)); /* the other scene's quads are not drawn */
    scene_entity_set_enabled(pair, scene_find(pair, name_hash("red")), false);
    TEST_ASSERT_TRUE(scene_activate(pair, SCENE_CAMERA_FIRST));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F)); /* each scene kept its own state */
}

static void
test_unloading_the_active_scene_stops_drawing_and_leaves_the_other_alone(void) {
    fixture();
    scene_t* pair = show("test_pair", SCENE_CAMERA_FIRST);
    scene_t* solo = load("test_solo");
    TEST_ASSERT_TRUE(scene_has_active_camera());
    scene_unload(pair);
    TEST_ASSERT_FALSE(scene_has_active_camera());
    TEST_ASSERT_TRUE(scene_activate(solo, SCENE_CAMERA_FIRST));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x0000FF), pixel(0.0F));
}

static void
test_activating_a_camera_the_scene_does_not_have_changes_nothing(void) {
    fixture();
    scene_t* pair = show("test_pair", SCENE_CAMERA_FIRST);
    TEST_ASSERT_FALSE(scene_activate(pair, name_hash("red"))); /* an entity, but not a camera */
    TEST_ASSERT_FALSE(scene_activate(pair, name_hash("nothing")));
    TEST_ASSERT_TRUE(scene_has_active_camera());
}

static void
test_the_active_camera_flies_its_path_by_the_time_the_shell_gives_it(void) {
    fixture();
    show("test_flight", SCENE_CAMERA_FIRST);
    frame(500);
    /* halfway: 2.5 units along x, so the quad is 2.5 units to the left */
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.5F));
    frame(250); /* 3.75 units along, the quad further left still */
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-1.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-3.75F));
}

/* The shell draws in scene_render(), while the last frame is being sent, and
 * upscales in scene_compose(): time passes once, and the picture is the one
 * scene_compose() alone would have made. */
static void
test_a_frame_drawn_in_two_steps_is_the_frame_drawn_in_one(void) {
    fixture();
    show("test_flight", SCENE_CAMERA_FIRST);
    scene_render(500, 0, SIZE, SIZE);
    scene_compose(500, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.5F));
    scene_render(250, 0, SIZE, SIZE);
    scene_compose(250, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-3.75F));
}

static void
test_a_paused_scene_neither_advances_nor_draws_until_resumed(void) {
    fixture();
    show("test_flight", SCENE_CAMERA_FIRST);
    scene_set_paused(true);
    TEST_ASSERT_FALSE(scene_has_active_camera());
    frame(500);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(-2.5F));
    scene_render(500, 0, SIZE, SIZE); /* the shell's overlapped half counts no time either */
    scene_set_paused(false);
    frame(500);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.5F)); /* the paused 500 ms did not count */
}

static void
test_a_scale_renders_smaller_and_the_picture_is_upscaled_to_the_target(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);
    render_context_set_scale(render_context_main(), 50);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(4.0F));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(-4.0F));
}

static void
test_the_stats_count_what_the_last_draw_kept(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    frame(16);
    TEST_ASSERT_EQUAL_INT(4,
                          render_context_frame(render_context_main()).stats.triangles); /* two quads of two triangles */
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("green")), false);
    frame(16);
    TEST_ASSERT_EQUAL_INT(2, render_context_frame(render_context_main()).stats.triangles);
}

/* An app leaving: nothing it loaded outlives it, and the raster's scratch
 * goes too. */
static void
test_leaving_the_app_unloads_every_scene_and_frees_the_scratch(void) {
    fixture();
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    show("test_pair", SCENE_CAMERA_FIRST);
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
    show("test_pair", SCENE_CAMERA_FIRST);
    for (int i = 0; i < SIZE * SIZE; i++) {
        fx.pixels[i] = SENTINEL;
    }
    scene_render(16, 0, SIZE, SIZE);
    TEST_ASSERT_EQUAL_INT(4, render_context_frame(render_context_main()).stats.triangles);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
    scene_compose(16, 0, &fx.target);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
}

/* Band mode has no framebuffer: the scene still draws, and the compose that
 * follows has nowhere to upscale to and writes nothing. */
static void
test_a_compose_with_no_framebuffer_writes_nothing(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);
    for (int i = 0; i < SIZE * SIZE; i++) {
        fx.pixels[i] = SENTINEL;
    }
    scene_render(16, 0, SIZE, SIZE);
    const scene_target_t none = {NULL, SIZE, SIZE, NULL, NULL};
    scene_compose(16, 0, &none);
    TEST_ASSERT_EQUAL_INT(4, render_context_frame(render_context_main()).stats.triangles);
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, pixel(0.0F));
    frame(16); /* and the next frame, with a framebuffer again, is whole */
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
}

static void
test_leaving_an_app_while_paused_lifts_the_pause(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);
    scene_set_paused(true);
    scene_unload_all();
    show("test_pair", SCENE_CAMERA_FIRST);
    TEST_ASSERT_TRUE(scene_has_active_camera());
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
}

static void
test_a_scene_with_two_cameras_is_seen_from_the_one_activated_by_name(void) {
    fixture();
    scene_t* twin = load("test_twin");
    TEST_ASSERT_TRUE(scene_activate(twin, name_hash("right")));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(CLEAR, pixel(0.0F)); /* the quad is 4 units left of this camera */
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-4.0F));
    TEST_ASSERT_TRUE(scene_activate(twin, name_hash("left")));
    render_context_set_scale(render_context_main(), 100);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
    TEST_ASSERT_TRUE(scene_activate(twin, SCENE_CAMERA_FIRST)); /* the first */
    TEST_ASSERT_FALSE(scene_activate(twin, name_hash("red")));
}

static void
test_a_camera_without_a_path_follows_its_entitys_transform(void) {
    fixture();
    scene_t* pair = show("test_pair", name_hash("camera"));
    transformf_t aside = TRANSFORMF_IDENTITY;
    transformf_set_position(&aside, (vec3f_t){4, 0, -10});
    scene_entity_set_local(pair, scene_find(pair, name_hash("camera")), aside);
    frame(16);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0x00FF00), pixel(0.0F)); /* the green quad is 4 units right of the origin */
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-4.0F));
}

static void
test_the_render_scale_changes_the_picture_and_a_larger_one_grows_the_scratch(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);
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
 * `first`, so the census is taken there, and the budget picks the step; with
 * no hold, a step that fits finer is taken on the first frame. */
static void
predicted_frame(int first, int32_t budget_us) {
    resolution_config_t config = resolution_config(LADDER, 3, 3, budget_us);
    config.up_hold_us = 0;
    render_context_set_dynamic_resolution(render_context_main(), &config, &PIXEL_MODEL, first);
    frame(0);
}

/* A draw is censused where the predictor stood and drawn where it chose, from
 * the list the census kept: finer (one step at a time), coarser or two steps
 * coarser, the picture and the stats are those of a draw at the chosen size
 * alone. */
static void
test_a_predicted_draw_is_the_fixed_draw_at_the_step_it_chose(void) {
    fixture();
    show("test_pair", SCENE_CAMERA_FIRST);

    const struct {
        int first;
        int32_t budget_us;
        int chosen;
    } cases[] = {{0, 700, 1}, {2, 700, 1}, {0, 300, 2}, {2, 2000, 1}, {1, 2000, 0}, {1, 300, 2}};

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
        TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(0.0F));
        TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, fx.pixels, sizeof(*fx.held) * SIZE * SIZE));
    }
}

/* The scratch block is taken once for the finest step, wherever the
 * predictor stands when it is taken. */
static void
test_the_scratch_is_the_finest_steps_raster_block(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    r3d_instance_t* instances = fx.instances;
    for (int i = 0; i < INSTANCES_MAX; i++) {
        instances[i] = (r3d_instance_t){&scene->renderers[0].mesh, NULL};
    }
    const resolution_config_t config = resolution_config(LADDER, 3, 3, 2000);
    render_context_t* c = render_context_main();
    render_context_set_dynamic_resolution(c, &config, &PIXEL_MODEL, 2);
    const transformf_t camera_pose = scene_entity_world(scene, scene->cameras[0].entity);
    const render_view_t view =
        camera_view(scene_camera_lens(scene, SCENE_CAMERA_FIRST), &camera_pose, (viewport_t){SIZE, SIZE, 0});
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_TRUE(render_context_draw(c, instances, INSTANCES_MAX, &view, 0));
    const size_t taken = before - memory_free_bytes(MEMORY_PSRAM);

    raster_t finest = c->raster;
    finest.width = LADDER[0].width;
    finest.height = LADDER[0].height;
    const size_t needed = raster_scratch_bytes(&finest);
    TEST_ASSERT_TRUE(taken >= needed);
    TEST_ASSERT_TRUE(c->scratch_bytes == needed);
}

static void
test_unloading_the_first_of_two_scenes_leaves_the_second_with_its_own_time(void) {
    fixture();
    scene_t* pair = load("test_pair");
    scene_t* flight = show("test_flight", SCENE_CAMERA_FIRST);
    frame(250);
    scene_unload(pair);
    TEST_ASSERT_EQUAL_INT(1, scene_loaded_count());
    TEST_ASSERT_TRUE(scene_loaded_at(0) == flight);
    frame(250);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.5F)); /* 500 ms of flight */
}

static void
test_a_loaded_scene_that_is_not_active_keeps_its_time(void) {
    fixture();
    scene_t* flight = load("test_flight");
    show("test_pair", SCENE_CAMERA_FIRST);
    frame(500);
    TEST_ASSERT_TRUE(scene_activate(flight, SCENE_CAMERA_FIRST));
    render_context_set_scale(render_context_main(), 100);
    frame(0);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(0xFF0000), pixel(-2.5F)); /* it flew while another was seen */
}

#ifndef DEVICE_BUILD

int
display_quarter_now(void) {
    return DISPLAY_PORTRAIT;
}

static void
expanded_fixture(void) {
    fixture();
    TEST_ASSERT_TRUE(gfx_init());
    show("test_pair", SCENE_CAMERA_FIRST);
    free(fx.pixels);
    free(fx.held);
    fx.pixels = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    fx.held = memory_alloc(GFX_FRAMEBUFFER_BYTES, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(fx.pixels);
    TEST_ASSERT_NOT_NULL(fx.held);
    gfx_set_frame_overlay(NULL);
}

static void
expanded_picture(void) {
    expanded_fixture();
    render_context_set_scale(render_context_main(), 50);
    scene_render(16, 0, GFX_WIDTH, GFX_HEIGHT);
    raster_t* raster = &render_context_main()->raster;
    raster_upscale(raster, fx.held, GFX_WIDTH, GFX_HEIGHT);
    const size_t before_half = memory_free_bytes(MEMORY_PSRAM);
    const scene_target_t half = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
    TEST_ASSERT_NOT_NULL(half.half_pixels);
    TEST_ASSERT_TRUE(half.half_pixels != half.pixels);
    TEST_ASSERT_TRUE(scene_compose(16, 0, &half));
    gfx_expand_frame();
    TEST_ASSERT_TRUE(gfx_frame_expanded());
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
    }
    TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, fx.pixels, GFX_FRAMEBUFFER_BYTES));
    TEST_ASSERT_EQUAL_HEX16(CLEAR, fx.pixels[0]);
    TEST_ASSERT_TRUE(fx.pixels[(GFX_HEIGHT / 2) * GFX_WIDTH + GFX_WIDTH / 2] != CLEAR);
    gfx_mode_exit();
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) == before_half);
}

static void
test_expanded_scene_in_separate_picture_matches_upscale(void) {
    expanded_picture();
}

/* Panel row `panel_row` of a frame whose half rows are all `line`: each side
 * of a pixel blends unless a seam in `across` or the panel edge is there. */
static void
check_scattered_seams(int panel_row, const gfx_color_t* line, const uint8_t* across, gfx_color_t* row) {
    enum { HALF_WIDTH = GFX_WIDTH / 2, NEIGHBOUR = RGB565_SCALE_UNIT / 4 };

    gfx_read_panel_row(panel_row, row);
    for (int x = 0; x < HALF_WIDTH; x++) {
        const bool left = x > 0 && !gfx_seam_at(across, x - 1);
        const bool right = x + 1 < HALF_WIDTH && !gfx_seam_at(across, x);
        TEST_ASSERT_EQUAL_HEX16(left ? rgb565be_blend(line[x], line[x - 1], NEIGHBOUR) : line[x], row[2 * x]);
        TEST_ASSERT_EQUAL_HEX16(right ? rgb565be_blend(line[x], line[x + 1], NEIGHBOUR) : line[x], row[(2 * x) + 1]);
    }
}

static void
test_blended_expand_blends_within_seams_and_doubles_across_them(void) {
    enum { HALF_WIDTH = GFX_WIDTH / 2, HALF_HEIGHT = GFX_HEIGHT / 2, NEIGHBOUR = RGB565_SCALE_UNIT / 4 };

    expanded_fixture();
    gfx_color_t* half = gfx_half_picture();
    uint8_t* seams = gfx_half_seams();
    TEST_ASSERT_NOT_NULL(seams);
    /* Every row alike, so only the horizontal blend shows; random colours, so
     * a quarter toward a neighbour never rounds back to the pixel itself. */
    srand(3);
    for (int x = 0; x < HALF_WIDTH; x++) {
        half[x] = (gfx_color_t)rand();
    }
    for (int y = 1; y < HALF_HEIGHT; y++) {
        memcpy(half + ((size_t)y * HALF_WIDTH), half, HALF_WIDTH * sizeof(*half));
    }
    gfx_color_t* row = malloc(sizeof(*row) * GFX_WIDTH);
    TEST_ASSERT_NOT_NULL(row);
    const gfx_color_t* line = half + HALF_WIDTH;

    memset(seams, 0, GFX_SEAM_BYTES(HALF_WIDTH, HALF_HEIGHT));
    gfx_expand_frame_blended();
    gfx_read_panel_row(2, row);
    TEST_ASSERT_EQUAL_HEX16(line[0], row[0]);
    TEST_ASSERT_EQUAL_HEX16(line[HALF_WIDTH - 1], row[GFX_WIDTH - 1]);
    for (int x = 1; x + 1 < HALF_WIDTH; x++) {
        TEST_ASSERT_EQUAL_HEX16(rgb565be_blend(line[x], line[x - 1], NEIGHBOUR), row[2 * x]);
        TEST_ASSERT_EQUAL_HEX16(rgb565be_blend(line[x], line[x + 1], NEIGHBOUR), row[(2 * x) + 1]);
    }

    memset(seams, UINT8_MAX, GFX_SEAM_BYTES(HALF_WIDTH, HALF_HEIGHT));
    gfx_expand_frame_blended();
    gfx_read_panel_row(2, row);
    for (int x = 0; x < GFX_WIDTH; x++) {
        TEST_ASSERT_EQUAL_HEX16(line[x / 2], row[x]);
    }

    /* Seams scattered, through a few seeds and both panel rows of half row 1. */
    for (unsigned seed = 7; seed < 10; seed++) {
        srand(seed);
        for (size_t i = 0; i < GFX_SEAM_BYTES(HALF_WIDTH, HALF_HEIGHT); i++) {
            seams[i] = (uint8_t)rand();
        }
        gfx_expand_frame_blended();
        check_scattered_seams(2, line, seams + GFX_SEAM_PITCH(HALF_WIDTH), row);
        check_scattered_seams(3, line, seams + GFX_SEAM_PITCH(HALF_WIDTH), row);
    }
    free(row);
}

static void
test_expanded_frame_lifecycle_and_raw_draw_guard(void) {
    expanded_fixture();
    gfx_color_t* half = gfx_half_picture();
    half[0] = 0x1234;
    gfx_expand_frame();
    TEST_ASSERT_TRUE(gfx_frame_expanded());
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
    const unsigned trips = gfx_fb_guard_trips_for_test();
    TEST_ASSERT_NULL(gfx_framebuffer());
    TEST_ASSERT_EQUAL_UINT(trips + 1, gfx_fb_guard_trips_for_test());
    draw_pixel(0, 0, 0x5678);
    TEST_ASSERT_EQUAL_UINT(trips + 2, gfx_fb_guard_trips_for_test());
    TEST_ASSERT_EQUAL_HEX16(0x1234, half[0]);
    gfx_present_begin();
    TEST_ASSERT_TRUE(gfx_frame_expanded());
    gfx_present_wait();
    TEST_ASSERT_FALSE(gfx_frame_expanded());
    gfx_expand_frame();
    gfx_mode_exit();
    TEST_ASSERT_FALSE(gfx_frame_expanded());
    TEST_ASSERT_NOT_NULL(gfx_half_picture());
    gfx_expand_frame();
    gfx_reset_for_test();
    TEST_ASSERT_FALSE(gfx_frame_expanded());
    TEST_ASSERT_TRUE(gfx_init());
    const gfx_mode_request_t bands = {.layout = GFX_LAYOUT_BANDS};
    gfx_mode_enter(&bands);
    TEST_ASSERT_NULL(gfx_half_picture());
}

static void
test_failed_half_allocation_keeps_full_upscale_unexpanded(void) {
    expanded_fixture();
    render_context_set_scale(render_context_main(), 50);
    scene_shell_render(16);
    raster_upscale(&render_context_main()->raster, fx.held, GFX_WIDTH, GFX_HEIGHT);
    gfx_color_t* full = gfx_framebuffer();
    void* pressure = memory_alloc(memory_largest_block(MEMORY_PSRAM), MEMORY_PSRAM);
    const size_t remaining = memory_largest_block(MEMORY_PSRAM);
    gfx_color_t* half = gfx_half_picture();
    scene_shell_compose(16);
    memory_free(pressure);
    TEST_ASSERT_TRUE(remaining < sizeof(gfx_color_t) * (GFX_WIDTH / 2) * (GFX_HEIGHT / 2));
    TEST_ASSERT_NULL(half);
    TEST_ASSERT_FALSE(gfx_frame_expanded());
    TEST_ASSERT_TRUE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT));
    TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, full, GFX_FRAMEBUFFER_BYTES));
}

/* The panel doubles the half picture's rows, whatever height was drawn: a
 * height that divides the half repeats every row alike, an uneven one
 * through the row map. */
static void
check_half_composed(render_context_t* c, int quarter, const scene_target_t* target) {
    TEST_ASSERT_TRUE(raster_fits_half_picture(&c->raster));
    raster_upscale(&c->raster, fx.held, GFX_WIDTH / 2, GFX_HEIGHT / 2);
    TEST_ASSERT_TRUE(scene_compose(0, quarter, target));
    gfx_expand_frame();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
        for (int x = 0; x < GFX_WIDTH; x++) {
            TEST_ASSERT_EQUAL_HEX16(fx.held[(y / 2) * (GFX_WIDTH / 2) + x / 2], fx.pixels[y * GFX_WIDTH + x]);
        }
    }
}

static void
test_camera_present_composes_half_picture_at_any_height(void) {
    expanded_fixture();
    render_context_t* c = render_context_main();
    render_context_set_scale(c, RENDER_CONTEXT_DEFAULT_SCALE_PERCENT);
    TEST_ASSERT_TRUE(scene_camera_set_present(scene_loaded_at(0), scene_find(scene_loaded_at(0), name_hash("camera")),
                                              CAMERA_PRESENT_SCANLINE));
    const scene_target_t target = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
    TEST_ASSERT_NOT_NULL(target.half_pixels);
    for (int quarter = 0; quarter <= 1; quarter++) {
        scene_render(0, quarter, GFX_WIDTH, GFX_HEIGHT);
        TEST_ASSERT_EQUAL_INT(GFX_WIDTH / 2, c->raster.width);
        TEST_ASSERT_EQUAL_INT(GFX_HEIGHT / 4, c->raster.height);
        check_half_composed(c, quarter, &target);
    }
    const resolution_step_t uneven = {GFX_WIDTH / 2, 179};
    const resolution_config_t one = resolution_config(&uneven, 1, 1, INT32_MAX);
    render_context_set_dynamic_resolution(c, &one, NULL, 0);
    scene_render(0, 0, GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(uneven.height / 2, c->raster.height);
    TEST_ASSERT_NOT_EQUAL(0, (GFX_HEIGHT / 2) % c->raster.height);
    check_half_composed(c, 0, &target);
}

static void
assert_composed_scanlines(render_context_t* context, int quarter, camera_present_t present) {
    scene_render(0, quarter, GFX_WIDTH, GFX_HEIGHT);
    raster_t* raster = &context->raster;
    memset(raster_depth(raster), 0, sizeof(uint16_t) * raster->width * raster->height);
    const scene_target_t target = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
    const bool half = scene_compose(0, quarter, &target);
    const resolution_step_t size = raster_compose_size(raster);
    const int run = size.height / raster->height;
    TEST_ASSERT_EQUAL_INT(0, run % 2);
    const uint16_t* pixels = half ? target.half_pixels : target.pixels;
    const uint16_t bright = pixels[0];
    TEST_ASSERT_EQUAL_HEX16(raster->clear, bright);
    for (int y = 0; y < size.height; y++) {
        const bool dark = y % run >= run / 2;
        for (int x = 0; x < size.width; x++) {
            const uint16_t pixel = pixels[y * size.width + x];
            if (present == CAMERA_PRESENT_SCANLINE && dark) {
                TEST_ASSERT_TRUE(rgb565_swap(pixel) < rgb565_swap(bright));
                TEST_ASSERT_EQUAL_HEX16(rgb565be_scale(bright, UPSCALE_SCANLINE_KEEP), pixel);
            } else {
                TEST_ASSERT_EQUAL_HEX16(bright, pixel);
            }
        }
    }
}

static void
test_camera_present_scanline_and_double_compose_both_quarters(void) {
    expanded_fixture();
    render_context_t* context = render_context_main();
    render_context_set_scale(context, RENDER_CONTEXT_DEFAULT_SCALE_PERCENT);
    for (int quarter = 0; quarter <= 1; quarter++) {
        for (camera_present_t present = CAMERA_PRESENT_SCANLINE; present <= CAMERA_PRESENT_DOUBLE; present++) {
            TEST_ASSERT_TRUE(scene_camera_set_present(scene_loaded_at(0),
                                                      scene_find(scene_loaded_at(0), name_hash("camera")), present));
            assert_composed_scanlines(context, quarter, present);
        }
    }
}

static void
test_camera_present_default_setter_and_dynamic_size(void) {
    expanded_fixture();
    scene_t* scene = scene_loaded_at(0);
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_NEAREST, scene_camera_lens(scene, SCENE_CAMERA_FIRST)->present);
    TEST_ASSERT_FALSE(scene_camera_set_present(scene, scene_find(scene, name_hash("red")), CAMERA_PRESENT_SCANLINE));
    TEST_ASSERT_FALSE(scene_camera_set_present(scene, SCENE_ENTITY_NONE, CAMERA_PRESENT_SCANLINE));
    TEST_ASSERT_TRUE(scene_camera_set_present(scene, camera, CAMERA_PRESENT_SCANLINE));
    assert_composed_scanlines(render_context_main(), 0, CAMERA_PRESENT_SCANLINE);
    const resolution_step_t step = {GFX_WIDTH, GFX_HEIGHT};
    const resolution_config_t config = resolution_config(&step, 1, 1, INT32_MAX);
    render_context_set_dynamic_resolution(render_context_main(), &config, NULL, 0);
    assert_composed_scanlines(render_context_main(), 0, CAMERA_PRESENT_SCANLINE);
}

static void
test_camera_small_fill_changes_scene_pixels(void) {
    expanded_fixture();
    scene_t* scene = scene_loaded_at(0);
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    static const uint8_t colors[QUAD_VERTICES][3] = {{32, 80, 96}, {128, 112, 48}, {224, 160, 144}, {48, 96, 80}};
    for (int i = 0; i < scene->renderer_count; i++) {
        scene->renderers[i].mesh.colors = colors;
    }
    render_context_t* context = render_context_main();
    scene_render(0, 0, SIZE, SIZE);
    memcpy(fx.held, raster_color(&context->raster), SIZE * SIZE * sizeof(*fx.held));
    TEST_ASSERT_TRUE(scene_camera_set_small_fill(scene, camera, SMALL_FILL_SMOOTH));
    scene_render(0, 0, SIZE, SIZE);
    TEST_ASSERT_EQUAL_MEMORY(fx.held, raster_color(&context->raster), SIZE * SIZE * sizeof(*fx.held));
    uint32_t checksums[SMALL_FILL_STYLE_COUNT];
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        TEST_ASSERT_TRUE(scene_camera_set_small_fill(scene, camera, (small_fill_t){SMALL_FILL_CEILING_FAST, 0, style}));
        scene_render(0, 0, SIZE, SIZE);
        checksums[style] = crc32_update(0, raster_color(&context->raster), SIZE * SIZE * sizeof(uint16_t));
        for (int previous = SMALL_FILL_FLAT; previous < (int)style; previous++) {
            TEST_ASSERT_NOT_EQUAL(checksums[previous], checksums[style]);
        }
    }
}

static void
test_camera_small_fill_default_setter_and_view(void) {
    expanded_fixture();
    scene_t* scene = scene_loaded_at(0);
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    const camera_t* lens = scene_camera_lens(scene, SCENE_CAMERA_FIRST);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.ceiling, lens->small_fill.ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.gate, lens->small_fill.gate);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_FLAT, lens->small_fill.style);
    const small_fill_t limit = {SMALL_FILL_CEILING_MAX, 0, SMALL_FILL_DITHER};
    TEST_ASSERT_TRUE(scene_camera_set_small_fill(scene, camera, limit));
    TEST_ASSERT_FALSE(
        scene_camera_set_small_fill(scene, camera, (small_fill_t){SMALL_FILL_CEILING_MAX + 1, 0, SMALL_FILL_DITHER}));
    TEST_ASSERT_FALSE(scene_camera_set_small_fill(scene, camera, (small_fill_t){UINT8_MAX, 0, SMALL_FILL_DITHER}));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_MAX, lens->small_fill.ceiling);
    TEST_ASSERT_TRUE(scene_camera_set_small_fill(scene, camera, (small_fill_t){0, 0, SMALL_FILL_FLAT}));
    const small_fill_t fill = SMALL_FILL_FAST;
    TEST_ASSERT_FALSE(scene_camera_set_small_fill(NULL, camera, fill));
    TEST_ASSERT_FALSE(scene_camera_set_small_fill(scene, scene_find(scene, name_hash("red")), fill));
    TEST_ASSERT_FALSE(scene_camera_set_small_fill(scene, SCENE_ENTITY_NONE, fill));
    TEST_ASSERT_FALSE(scene_camera_set_small_fill(scene, camera, (small_fill_t){4, 0, SMALL_FILL_STYLE_COUNT}));
    TEST_ASSERT_TRUE(scene_camera_set_small_fill(scene, camera, fill));
    const transformf_t pose = TRANSFORMF_IDENTITY;
    const render_view_t view = camera_view(lens, &pose, (viewport_t){64, 48, 0});
    TEST_ASSERT_EQUAL_INT(fill.ceiling, view.small_fill.ceiling);
    TEST_ASSERT_EQUAL_INT(fill.gate, view.small_fill.gate);
    TEST_ASSERT_EQUAL_INT(fill.style, view.small_fill.style);
    TEST_ASSERT_EQUAL_INT(fill.ceiling, lens->small_fill.ceiling);
    TEST_ASSERT_EQUAL_INT(fill.gate, lens->small_fill.gate);
    TEST_ASSERT_EQUAL_INT(fill.style, lens->small_fill.style);
}

static void
test_non_half_compose_upscales_to_the_full_destination(void) {
    expanded_fixture();
    render_context_set_scale(render_context_main(), 75);
    scene_render(16, 0, GFX_WIDTH, GFX_HEIGHT);
    render_context_t* context = render_context_main();
    raster_upscale(&context->raster, fx.held, GFX_WIDTH, GFX_HEIGHT);
    const scene_target_t target = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
    target.half_pixels[0] = SENTINEL;
    TEST_ASSERT_FALSE(scene_compose(16, 0, &target));
    TEST_ASSERT_EQUAL_HEX16(SENTINEL, target.half_pixels[0]);
    TEST_ASSERT_FALSE(gfx_frame_expanded());
    TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, gfx_framebuffer(), GFX_FRAMEBUFFER_BYTES));
}

static void
build_expanded_ui(ui_text_style_t style) {
    ui_set_text_style(style);
    const input_t input = {0};
    ui_begin(&input);
    mu_Context* ctx = ui_context();
    mu_begin_window_ex(ctx, "expanded", mu_rect(11, 19, 200, 110),
                       MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME);
    mu_draw_rect(ctx, mu_rect(13, 28, 197, 67), ui_rgb(0xAABBCC));
    mu_draw_text(ctx, NULL, "strip replay", -1, mu_vec2(17, 53), ui_rgb(0xFFBB00));
    mu_end_window(ctx);
    ui_end(UI_NO_BACKGROUND);
}

static void
clip_overlay(int row0, int row1) {
    const recti_t actual = gfx_clip_save();
    const recti_t expected = GFX_SCREEN_RECT;
    TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof actual);
    draw_set_clip(0, row0, GFX_WIDTH, row1 - row0);
    draw_rect(0, row0, GFX_WIDTH, row1 - row0, UINT16_MAX);
}

static void
test_expanded_overlay_resets_and_restores_clip(void) {
    expanded_fixture();
    TEST_ASSERT_NOT_NULL(gfx_half_picture());
    gfx_expand_frame();
    const recti_t narrow = {GFX_WIDTH / 4, GFX_HEIGHT / 4, GFX_WIDTH / 4, GFX_HEIGHT / 4};
    draw_set_clip(narrow.x, narrow.y, narrow.w, narrow.h);
    gfx_set_frame_overlay(clip_overlay);
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
        const recti_t actual = gfx_clip_save();
        TEST_ASSERT_EQUAL_MEMORY(&narrow, &actual, sizeof actual);
        for (int x = 0; x < GFX_WIDTH; x++) {
            TEST_ASSERT_EQUAL_HEX16(UINT16_MAX, fx.pixels[y * GFX_WIDTH + x]);
        }
    }
    gfx_set_frame_overlay(NULL);
}

static void
test_ui_replay_over_expanded_scene_matches_full_frame(void) {
    expanded_picture();
    for (int style = UI_TEXT_PLAIN; style <= UI_TEXT_SHADOWED; style++) {
        for (int quarter = 0; quarter < 4; quarter++) {
            raster_upscale(&render_context_main()->raster, gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT);
            ui_init();
            ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
            build_expanded_ui((ui_text_style_t)style);
            memcpy(fx.held, gfx_framebuffer(), GFX_FRAMEBUFFER_BYTES);
            scene_render(0, 0, GFX_WIDTH, GFX_HEIGHT);
            const scene_target_t half = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
            TEST_ASSERT_TRUE(scene_compose(0, 0, &half));
            gfx_expand_frame();
            gfx_set_frame_overlay(ui_replay_band);
            const unsigned trips = gfx_fb_guard_trips_for_test();
            ui_init();
            ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
            build_expanded_ui((ui_text_style_t)style);
            for (int y = 0; y < GFX_HEIGHT; y++) {
                gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
            }
            TEST_ASSERT_EQUAL_UINT(trips, gfx_fb_guard_trips_for_test());
            TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, fx.pixels, GFX_FRAMEBUFFER_BYTES));
            gfx_present();
        }
    }
}

static void
test_shell_expanded_pass_replays_build_mark_without_raw_writes(void) {
    expanded_fixture();
    ui_init();
    shell_frame_init("1234567");
    gfx_set_frame_overlay(shell_frame_overlay);
    render_context_set_scale(render_context_main(), 50);
    scene_shell_render(16);
    scene_shell_compose(16);
    TEST_ASSERT_TRUE(gfx_frame_expanded());
    const unsigned trips = gfx_fb_guard_trips_for_test();
    build_expanded_ui(UI_TEXT_OUTLINED);
    shell_frame_extras();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
    }
    TEST_ASSERT_EQUAL_UINT(trips, gfx_fb_guard_trips_for_test());
    gfx_set_frame_overlay(ui_replay_band);
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, fx.held + (size_t)y * GFX_WIDTH);
    }
    TEST_ASSERT_TRUE(memcmp(fx.held, fx.pixels, GFX_FRAMEBUFFER_BYTES) != 0);
    gfx_present();
}

static void
test_paused_or_empty_expanded_scene_retains_its_picture(void) {
    expanded_picture();
    gfx_present();
    for (int pause = 1; pause >= 0; pause--) {
        scene_set_paused(pause != 0);
        if (!pause) {
            scene_t* scene = scene_loaded_at(0);
            scene_entity_set_enabled(scene, scene_find(scene, name_hash("red")), false);
            scene_entity_set_enabled(scene, scene_find(scene, name_hash("green")), false);
        }
        scene_shell_render(16);
        scene_shell_compose(16);
        TEST_ASSERT_TRUE(gfx_frame_expanded());
        for (int y = 0; y < GFX_HEIGHT; y++) {
            gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
        }
        TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, fx.pixels, GFX_FRAMEBUFFER_BYTES));
        gfx_present();
    }
    const scene_target_t full = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, NULL, NULL};
    TEST_ASSERT_FALSE(scene_compose(16, 0, &full));
    const scene_target_t half = {full.pixels, GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
    TEST_ASSERT_FALSE(scene_compose(16, 0, &half));
    scene_t* scene = scene_loaded_at(0);
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("red")), true);
    TEST_ASSERT_TRUE(scene_compose(16, 0, &half));
    scene_unload(scene);
    TEST_ASSERT_FALSE(scene_compose(16, 0, &half));
}

static void
test_expanded_frame_without_ui_does_not_replay_the_previous_hud(void) {
    expanded_picture();
    scene_render(0, 0, GFX_WIDTH, GFX_HEIGHT);
    const scene_target_t half = {gfx_framebuffer(), GFX_WIDTH, GFX_HEIGHT, gfx_half_picture(), NULL};
    TEST_ASSERT_TRUE(scene_compose(0, 0, &half));
    gfx_expand_frame();
    gfx_set_frame_overlay(ui_replay_band);
    ui_init();
    build_expanded_ui(UI_TEXT_OUTLINED);
    gfx_present();
    scene_render(0, 0, GFX_WIDTH, GFX_HEIGHT);
    ui_clear_band_overlay();
    TEST_ASSERT_TRUE(scene_compose(0, 0, &half));
    gfx_expand_frame();
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, fx.pixels + (size_t)y * GFX_WIDTH);
    }
    TEST_ASSERT_EQUAL_INT(0, memcmp(fx.held, fx.pixels, GFX_FRAMEBUFFER_BYTES));
}

#endif

static void
test_camera_aliases_advance_independently(void) {
    fixture();
    scene_t* scene = load("test_aliases");
    const scene_entity_t left = scene_find(scene, name_hash("camera")), right = scene_find(scene, name_hash("right"));
    TEST_ASSERT_TRUE(scene_animator_set_speed(scene, left, SCENE_ANIMATOR_CURRENT, 0.5F));
    scene_animation_step(scene, 400);
    TEST_ASSERT_EQUAL_FLOAT(1.0F, scene->locals[left].position.x);
    TEST_ASSERT_EQUAL_FLOAT(2.0F, scene->locals[right].position.x);
}

static void
test_camera_animator_api_controls_clip_time(void) {
    fixture();
    scene_t* scene = load("test_flight");
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_EQUAL_INT(1, scene_animator_clip_count(scene, camera));
    TEST_ASSERT_EQUAL_STRING("flight", scene_animator_clip_name(scene, camera, 0));
    TEST_ASSERT_FALSE(scene_animator_play(scene, camera, 0, 100));
    TEST_ASSERT_TRUE(scene_animator_play(scene, camera, 0, 0));
    TEST_ASSERT_TRUE(scene_animator_set_speed(scene, camera, SCENE_ANIMATOR_CURRENT, 0.5F));
    scene_animation_step(scene, 1000);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, scene_entity_local(scene, camera).position.x);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, camera, SCENE_ANIMATOR_CURRENT, true));
    scene_animation_step(scene, 1000);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, scene_entity_local(scene, camera).position.x);
    TEST_ASSERT_FALSE(scene_animator_play(scene, camera, 1, 0));
}

static scene_t*
rig_fixture(const char* skin_id, const char* paired_skin) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    const int renderer = scene_renderer_index(scene, entity);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(&fx.pack, "rig.mesh", &scene->renderers[renderer].mesh));
    static const char* const clips[] = {"rig.move", "rig.move"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, "rig", clips, 2, NULL));
    TEST_ASSERT_TRUE(scene_animator_play(scene, entity, 0, 0));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, entity, skin_id, NULL));
    const scene_entity_t green = scene_find(scene, name_hash("green"));
    scene_entity_set_enabled(scene, green, paired_skin != NULL);
    if (paired_skin != NULL) {
        TEST_ASSERT_TRUE(scene_animator_set_clips(scene, green, "rig", clips, 2, NULL));
        TEST_ASSERT_TRUE(scene_animator_play(scene, green, 0, 0));
        TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, green, paired_skin, NULL));
    }
    return scene;
}

static void
test_mesh_reader_refuses_overlapping_cluster_vertices(void) {
    fixture();
    asset_view_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&fx.pack, "rig.mesh", R3D_LIT_MESH_ASSET, &entry));
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_from_asset(&entry, &mesh));
    TEST_ASSERT_EQUAL_INT(mesh.clusters[0].vertex_count, mesh.clusters[1].vertex_first);
    r3d_lit_cluster_t* clusters = (r3d_lit_cluster_t*)(uintptr_t)mesh.clusters;
    clusters[0].vertex_count++;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, r3d_lit_mesh_from_asset(&entry, &mesh));
    TEST_ASSERT_NULL(mesh.positions);
    clusters[0].vertex_count--;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_from_asset(&entry, &mesh));
}

static void
test_mesh_reader_requires_complete_cluster_coverage(void) {
    fixture();
    asset_view_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&fx.pack, "rig.mesh", R3D_LIT_MESH_ASSET, &entry));
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_from_asset(&entry, &mesh));
    r3d_lit_cluster_t* clusters = (r3d_lit_cluster_t*)(uintptr_t)mesh.clusters;
    const int last = mesh.cluster_count - 1;
    for (int c = 0; c <= last; c++) {
        const r3d_lit_cluster_t saved = clusters[c];
        clusters[c].triangle_count = 0;
        clusters[c].vertex_count--;
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, r3d_lit_mesh_from_asset(&entry, &mesh));
        TEST_ASSERT_NULL(mesh.positions);
        clusters[c] = saved;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_from_asset(&entry, &mesh));
    }
    TEST_ASSERT_EQUAL_INT(0, mesh.clusters[0].vertex_first);
    TEST_ASSERT_EQUAL_INT(mesh.vertex_count, clusters[last].vertex_first + clusters[last].vertex_count);
}

static void
test_mesh_reader_accepts_only_positive_int_scales(void) {
    fixture();
    asset_view_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&fx.pack, "rig.mesh", R3D_LIT_MESH_ASSET, &entry));
    uint8_t* bytes = (uint8_t*)(uintptr_t)entry.data;
    r3d_lit_mesh_t mesh;
    test_pack_put32(bytes + MESH_SCALE_AT, INT_MAX);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_from_asset(&entry, &mesh));
    TEST_ASSERT_EQUAL_INT(INT_MAX, mesh.position_scale);
    test_pack_put32(bytes + MESH_SCALE_AT, (uint32_t)INT_MAX + 1U);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, r3d_lit_mesh_from_asset(&entry, &mesh));
    TEST_ASSERT_NULL(mesh.positions);
}

static void
test_clip_bind_failure_names_the_first_rejected_clip(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));

    static const struct {
        const char* skeleton;
        const char* clips[2];
        const char* missing;
    } cases[] = {{"rig", {"rig.move", "rig.back"}, "none"}, {NULL, {"property.move", "flight"}, "bad"}};

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        anim_tracks_t tracks;
        anim_binding_t binding;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_from_pack(&fx.pack, cases[i].clips[0], &tracks));
        TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(&tracks, 0, &binding));
        memcpy((char*)(uintptr_t)binding.path, cases[i].missing, strlen(cases[i].missing));
        TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, cases[i].skeleton, &cases[i].clips[1], 1, NULL));
        scene_failure_t why;
        TEST_ASSERT_FALSE(scene_animator_set_clips(scene, entity, cases[i].skeleton, cases[i].clips, 2, &why));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
        TEST_ASSERT_EQUAL_STRING(cases[i].clips[0], why.what);
        TEST_ASSERT_EQUAL_INT(1, scene_animator_clip_count(scene, entity));
        TEST_ASSERT_EQUAL_STRING(cases[i].clips[1], scene_animator_clip_name(scene, entity, 0));
    }
}

static void
test_skinned_scene_draws_clip_output_and_reskins_from_bind(void) {
    static const char* const skins[] = {"rig.skin2", "rig.skin4"};
    for (size_t k = 0; k < sizeof skins / sizeof skins[0]; k++) {
        scene_t* scene = rig_fixture(skins[k], NULL);
        const scene_entity_t entity = scene_find(scene, name_hash("red"));
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
        const r3d_lit_mesh_t* drawn = scene_entity_mesh(scene, entity);
        TEST_ASSERT_EQUAL_PTR(skin->output.positions, drawn->positions);
        TEST_ASSERT_NOT_EQUAL(skin->bind.positions, drawn->positions);
        TEST_ASSERT_EQUAL_PTR(skin->bind.colors, drawn->colors);
        frame(0);
        const uint16_t red = fx.pixels[CENTER * SIZE + CENTER];
        TEST_ASSERT_NOT_EQUAL_HEX16(RGB565BE(CLEAR_RGB), red);
        for (int step = 1; step <= 3; step++) {
            scene_animation_step(scene, RIG_DURATION_MS / RIG_TRAVEL);
            TEST_ASSERT_EQUAL_INT16(step - 1, drawn->positions[0][0]);
            TEST_ASSERT_EQUAL_INT16(step - 1, drawn->nodes[0].bounds.lo.x);
            TEST_ASSERT_EQUAL_INT16(step + 1, drawn->nodes[0].bounds.hi.x);
            frame(0);
            TEST_ASSERT_EQUAL_PTR(drawn, scene->instances[0].mesh);
            const int shifted = CENTER + step * SIZE / 20;
            TEST_ASSERT_EQUAL_HEX16(red, fx.pixels[CENTER * SIZE + shifted]);
        }
        TEST_ASSERT_EQUAL_HEX16(RGB565BE(CLEAR_RGB), fx.pixels[CENTER * SIZE + CENTER]);
        TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, entity, skins[k], NULL));
        TEST_ASSERT_EQUAL_INT16(2, scene_entity_mesh(scene, entity)->positions[0][0]);
        release_fixture();
    }
}

static void
test_skin_split_inside_an_entity_matches_full_range(void) {
    scene_t* scene = rig_fixture("rig.skin4", "rig.quad");
    const scene_entity_t red = scene_find(scene, name_hash("red")), green = scene_find(scene, name_hash("green"));
    scene_animate_range(scene, 0, scene->asset.entity_count, RIG_DURATION_MS / 2);
    scene_skin_range(scene, 0, 3);
    const scene_entity_t entities[] = {red, green};
    for (size_t e = 0; e < sizeof entities / sizeof entities[0]; e++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[entities[e]];
        r3d_skin_refit_nodes(&skin->output);
        const size_t bytes = r3d_skin_output_bytes(&skin->bind);
        fx.saved_skin[e] = malloc(bytes);
        TEST_ASSERT_NOT_NULL(fx.saved_skin[e]);
        memcpy(fx.saved_skin[e], skin->internal_storage, bytes);
        memset(skin->internal_storage, 0, bytes);
        TEST_ASSERT_TRUE(r3d_skin_output_init(&skin->bind, skin->internal_storage, bytes, &skin->output));
    }
    scene_skin_range(scene, 0, 1);
    scene_skin_range(scene, 1, 2);
    for (size_t e = 0; e < sizeof entities / sizeof entities[0]; e++) {
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[entities[e]];
        r3d_skin_refit_nodes(&skin->output);
        TEST_ASSERT_EQUAL_MEMORY(fx.saved_skin[e], skin->internal_storage, r3d_skin_output_bytes(&skin->bind));
    }
}

static void
test_attachment_count_mismatches_preserve_previous_components(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    const void* animator = scene->animators[entity].storage;
    const void* skin = scene->skinned_renderers[entity].internal_storage;
    static const char* const clips[] = {"rig.move"};
    scene_failure_t why;
    TEST_ASSERT_FALSE(scene_animator_set_clips(scene, entity, "rig.short", clips, 1, &why));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
    TEST_ASSERT_EQUAL_PTR(animator, scene->animators[entity].storage);
    static const char* const mismatches[] = {"rig.shortj", "rig.quad"};
    for (size_t i = 0; i < sizeof mismatches / sizeof mismatches[0]; i++) {
        TEST_ASSERT_FALSE(scene_renderer_set_skin(scene, entity, mismatches[i], &why));
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
        TEST_ASSERT_EQUAL_PTR(skin, scene->skinned_renderers[entity].internal_storage);
        TEST_ASSERT_EQUAL_PTR(animator, scene->animators[entity].storage);
    }
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, "rig", clips, 1, &why));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, entity, "rig.skin4", &why));
}

static void
test_scene_open_refuses_shear_and_reflection_naming_the_entity(void) {
    fixture();
    asset_view_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&fx.pack, "test_pair", SCENE_ASSET, &entry));
    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(entry, &asset));
    scene_asset_transform_t* placement = (scene_asset_transform_t*)(uintptr_t)&asset.transforms[1];
    const scene_asset_transform_t saved = *placement;
    for (int reflected = 0; reflected <= 1; reflected++) {
        *placement = saved;
        if (reflected) {
            placement->linear.m[0][0] = -1;
        } else {
            placement->linear.m[0][1] = 0.5F;
        }
        scene_failure_t why;
        TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_pair", &why));
        TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
        TEST_ASSERT_EQUAL_STRING("red", why.what);
    }
    *placement = saved;
    TEST_ASSERT_NOT_NULL(scene_load_from(&fx.pack, "test_pair", NULL));
}

static void
test_property_animation_moves_a_drawn_entity(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    scene_entity_set_enabled(scene, scene_find(scene, name_hash("green")), false);
    static const char* const clips[] = {"property.move"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, NULL, clips, 1, NULL));
    TEST_ASSERT_EQUAL_INT(0, scene->animators[entity].layer.shown.joint_count);
    TEST_ASSERT_TRUE(scene_animator_play(scene, entity, 0, 0));
    render_context_main()->split.top_offset[0] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, 0));
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MIN - JOB_SHARE_EVEN, render_context_main()->split.top_offset[0]);
    frame(0);
    const uint16_t red = fx.pixels[CENTER * SIZE + CENTER];
    frame(RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_FLOAT(RIG_TRAVEL / 2, scene_entity_local(scene, entity).position.x);
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(CLEAR_RGB), fx.pixels[CENTER * SIZE + CENTER]);
    TEST_ASSERT_EQUAL_HEX16(red, fx.pixels[CENTER * SIZE + CENTER + GREEN_PIXEL_OFFSET]);
    TEST_ASSERT_EQUAL_FLOAT(RIG_TRAVEL / 2, scene->instances[0].model->m[0][3]);
}

static void
test_paused_camera_seek_wraps_or_clamps_and_invalidates_the_entity_matrix(void) {
    fixture();
    scene_t* scene = show("test_flight", SCENE_CAMERA_FIRST);
    const scene_entity_t camera = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_TRUE(scene_entity_local(scene, camera).cached);
    const uint32_t duration_ms = scene_animator_clip_duration_ms(scene, camera, 0);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, camera, SCENE_ANIMATOR_CURRENT, true));
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, duration_ms + duration_ms / 2));
    transformf_t sampled = scene_entity_local(scene, camera);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, transformf_matrix(&sampled).m[0][3]);
    scene->animators[camera].layer.current.playhead.wrap = ANIM_CLAMP;
    TEST_ASSERT_TRUE(scene_animator_seek(scene, camera, SCENE_ANIMATOR_CURRENT, duration_ms + duration_ms / 2));
    TEST_ASSERT_EQUAL_FLOAT(5.0F, scene_entity_local(scene, camera).position.x);
    TEST_ASSERT_TRUE((scene->flags[camera] & SCENE_FLAG_DIRTY) != 0);
}

static void
test_inactive_property_target_invalidates_its_entity_matrix(void) {
    fixture();
    show("test_solo", SCENE_CAMERA_FIRST);
    scene_t* scene = load("test_pair");
    const scene_entity_t owner = scene_find(scene, name_hash("camera"));
    const scene_entity_t target = scene_find(scene, name_hash("red"));
    static const char* const clips[] = {"property.move"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, owner, NULL, clips, 1, NULL));
    TEST_ASSERT_TRUE(scene_animator_play(scene, owner, 0, 0));
    TEST_ASSERT_TRUE(scene_entity_local(scene, target).cached);
    scene->flags[target] &= (uint8_t)~SCENE_FLAG_DIRTY;
    render_context_main()->split.top_offset[0] = JOB_SHARE_MIN - JOB_SHARE_EVEN;
    TEST_ASSERT_TRUE(scene_animator_seek(scene, owner, SCENE_ANIMATOR_CURRENT, RIG_DURATION_MS / 2));
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MIN - JOB_SHARE_EVEN, render_context_main()->split.top_offset[0]);
    transformf_t sampled = scene_entity_local(scene, target);
    TEST_ASSERT_EQUAL_FLOAT(RIG_TRAVEL / 2, transformf_matrix(&sampled).m[0][3]);
    TEST_ASSERT_TRUE((scene->flags[target] & SCENE_FLAG_DIRTY) != 0);
}

static void
test_unchanged_poses_skip_skinning_and_refit(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    scene_animation_step(scene, RIG_DURATION_MS / 4);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    scene_animation_step(scene, RIG_DURATION_MS / 4);
    TEST_ASSERT_FALSE(scene->animators[entity].posed);
    TEST_ASSERT_EQUAL_INT16(0, scene_entity_mesh(scene, entity)->positions[0][0]);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, false));
    scene_entity_set_enabled(scene, entity, false);
    scene_animation_step(scene, RIG_DURATION_MS / 4);
    TEST_ASSERT_FALSE(scene->animators[entity].posed);
    TEST_ASSERT_EQUAL_UINT32(RIG_DURATION_MS / 4, scene->animators[entity].layer.current.playhead.t_ms);
    scene_entity_set_enabled(scene, entity, true);
    scene_animation_step(scene, RIG_DURATION_MS / 4);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_EQUAL_INT16(1, scene_entity_mesh(scene, entity)->positions[0][0]);
}

static void
test_previous_slot_pause_and_empty_or_frozen_controls(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    TEST_ASSERT_FALSE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_PREVIOUS, 0));
    TEST_ASSERT_FALSE(scene_animator_set_speed(scene, entity, SCENE_ANIMATOR_PREVIOUS, 1));
    TEST_ASSERT_FALSE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_PREVIOUS, true));
    scene_animation_step(scene, RIG_DURATION_MS / 4);
    TEST_ASSERT_TRUE(scene_animator_play(scene, entity, 1, RIG_DURATION_MS));
    TEST_ASSERT_TRUE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_PREVIOUS, RIG_DURATION_MS / 4));
    TEST_ASSERT_TRUE(scene_animator_set_speed(scene, entity, SCENE_ANIMATOR_PREVIOUS, 2));
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_PREVIOUS, true));
    scene_animation_step(scene, RIG_DURATION_MS / 4);
    TEST_ASSERT_EQUAL_UINT32(RIG_DURATION_MS / 4, scene->animators[entity].layer.previous.playhead.t_ms);
    TEST_ASSERT_EQUAL_UINT32(RIG_DURATION_MS / 4, scene->animators[entity].layer.current.playhead.t_ms);
    TEST_ASSERT_TRUE(scene_animator_play(scene, entity, 0, RIG_DURATION_MS));
    TEST_ASSERT_TRUE(scene->animators[entity].layer.frozen_previous);
    TEST_ASSERT_FALSE(scene_animator_seek(scene, entity, SCENE_ANIMATOR_PREVIOUS, 0));
    TEST_ASSERT_FALSE(scene_animator_set_speed(scene, entity, SCENE_ANIMATOR_PREVIOUS, 1));
    TEST_ASSERT_FALSE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_PREVIOUS, true));
}

static void
test_animator_queries_and_controls_refuse_missing_or_out_of_range(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), empty = scene_find(scene, name_hash("green"));
    TEST_ASSERT_EQUAL_INT(2, scene_animator_clip_count(scene, entity));
    TEST_ASSERT_EQUAL_STRING("rig.move", scene_animator_clip_name(scene, entity, 0));
    TEST_ASSERT_EQUAL_UINT32(RIG_DURATION_MS, scene_animator_clip_duration_ms(scene, entity, 0));
    TEST_ASSERT_EQUAL_INT(0, scene_animator_clip_count(scene, empty));
    TEST_ASSERT_NULL(scene_animator_clip_name(scene, empty, 0));
    TEST_ASSERT_EQUAL_UINT32(0, scene_animator_clip_duration_ms(scene, empty, 0));
    TEST_ASSERT_FALSE(scene_animator_play(scene, empty, 0, 0));
    TEST_ASSERT_FALSE(scene_animator_seek(scene, empty, SCENE_ANIMATOR_CURRENT, 0));
    TEST_ASSERT_FALSE(scene_animator_set_paused(scene, empty, SCENE_ANIMATOR_CURRENT, true));
    TEST_ASSERT_FALSE(scene_animator_set_speed(scene, empty, SCENE_ANIMATOR_CURRENT, 1));
    for (int clip = -1; clip <= 2; clip += 3) {
        TEST_ASSERT_NULL(scene_animator_clip_name(scene, entity, clip));
        TEST_ASSERT_EQUAL_UINT32(0, scene_animator_clip_duration_ms(scene, entity, clip));
        TEST_ASSERT_FALSE(scene_animator_play(scene, entity, clip, 0));
    }
}

static void
test_set_transform_reads_back_exactly_and_drives_placement(void) {
    fixture();
    scene_t* scene = show("test_pair", SCENE_CAMERA_FIRST);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, (vec3f_t){2, 3, 4});
    transformf_set_scale(&pose, (vec3f_t){2, 3, 4});
    transformf_set_rotation(&pose, quatf_from_axis_angle((vec3f_t){1, 2, 3}, 0.5F));
    scene_entity_set_local(scene, entity, pose);
    const transformf_t stored = scene_entity_local(scene, entity);

    TEST_ASSERT_EQUAL_MEMORY(&pose, &stored, sizeof pose);
    const mat4f_t want = transformf_compute_matrix(&pose);
    frame(0);
    const mat4f_t* model = scene->instances[0].model;
    TEST_ASSERT_EQUAL_MEMORY(&want, model, sizeof want);
}

static void
test_authored_skin_modes_animation_and_ambient(void) {
    const size_t before = memory_free_bytes(MEMORY_INTERNAL);
    static const uint8_t modes[] = {R3D_SKIN_LIGHT_DIRECT, R3D_SKIN_LIGHT_RENORMALISED, R3D_SKIN_LIGHT_TABLE};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        skin_fixture_options_t options = SKIN_OPTIONS;
        options.mode = modes[i];
        fixture_with_skin(&options);
        scene_t* scene = load("test_skin");
        const scene_entity_t entity = scene_find(scene, name_hash("red"));
        TEST_ASSERT_EQUAL_INT(modes[i], scene->skinned_renderers[entity].light_mode);
        TEST_ASSERT_EQUAL_INT(0, scene->light_count);
        TEST_ASSERT_NOT_NULL(scene->light_transfer);
        TEST_ASSERT_EQUAL_FLOAT(0.4F, scene->tonemap_white);
        TEST_ASSERT_EQUAL_INT(ANIM_CLAMP, scene->animators[entity].wrap);
        scene_animation_step(scene, RIG_DURATION_MS * 2);
        const r3d_lit_mesh_t* mesh = scene_entity_mesh(scene, entity);
        TEST_ASSERT_EQUAL_INT16(RIG_TRAVEL - 1, mesh->positions[0][0]);
        for (int channel = 0; channel < R3D_SKIN_LIGHT_CHANNELS; channel++) {
            const float ambient[] = {options.ambient.x, options.ambient.y, options.ambient.z};
            const float albedo =
                powf(scene->skinned_renderers[entity].bind.colors[0][channel] / (float)R3D_SKIN_LIGHT_BYTE_SCALE,
                     R3D_SKIN_LIGHT_GAMMA);
            const float radiance = albedo * ambient[channel] * options.intensity;
            const float mapped = radiance / (1.0F + radiance * scene->tonemap_white);
            const int expected = (int)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(mapped, 1 / R3D_SKIN_LIGHT_GAMMA));
            TEST_ASSERT_UINT8_WITHIN(2, expected, mesh->colors[0][channel]);
        }
        TEST_ASSERT_EQUAL_INT(modes[i] == R3D_SKIN_LIGHT_TABLE, scene->light_table_users);
        release_fixture();
    }
    skin_fixture_options_t options = SKIN_OPTIONS;
    options.default_clip = SCENE_ASSET_DEFAULT_CLIP_NONE;
    fixture_with_skin(&options);
    scene_t* rest = load("test_skin");
    scene_animation_step(rest, RIG_DURATION_MS);
    TEST_ASSERT_EQUAL_INT16(-1, scene_entity_mesh(rest, 0)->positions[0][0]);
    scene_unload(rest);
    release_fixture();
    options.default_clip = 1;
    fixture_with_skin(&options);
    scene_t* second = load("test_skin");
    scene_animation_step(second, RIG_DURATION_MS);
    TEST_ASSERT_EQUAL_INT16(-RIG_TRAVEL - 1, scene_entity_mesh(second, 0)->positions[0][0]);
    release_fixture();
    TEST_ASSERT_EQUAL_UINT(before, memory_free_bytes(MEMORY_INTERNAL));
}

static void
test_authored_tonemap_changes_skin_colors_in_both_attachment_orders(void) {
    enum { BYTE_TOLERANCE = 2, MINIMUM_TONEMAP_CHANGE = 32 };

    const float intensity = 4.0F, white = 4.0F;
    for (int runtime = 0; runtime < 2; runtime++) {
        uint8_t colors[2];
        for (int mapped = 0; mapped < 2; mapped++) {
            skin_fixture_options_t options = SKIN_OPTIONS;
            options.ambient = SCENE_LIGHT_COLOR_DEFAULT;
            options.intensity = runtime ? SCENE_AMBIENT_INTENSITY_NONE : intensity;
            options.tonemap_white = mapped ? white : SCENE_TONEMAP_WHITE_NONE;
            fixture_with_skin(&options);
            scene_t* scene = load("test_skin");
            const scene_entity_t entity = scene_find(scene, name_hash("red"));
            if (runtime) {
                TEST_ASSERT_NULL(scene->light_transfer);
                const scene_entity_t lamp = scene_find(scene, name_hash("lamp"));
                transformf_t pose = scene_entity_local(scene, lamp);
                transformf_set_rotation(&pose, quatf_from_axis_angle((vec3f_t){1, 0, 0}, MATH_PI / 2));
                scene_entity_set_local(scene, lamp, pose);
                TEST_ASSERT_TRUE(scene_light_set_intensity(scene, lamp, intensity));
            }
            scene_animation_step(scene, 0);
            const float albedo =
                powf(scene->skinned_renderers[entity].bind.colors[0][0] / (float)R3D_SKIN_LIGHT_BYTE_SCALE,
                     R3D_SKIN_LIGHT_GAMMA);
            const float radiance = albedo * intensity;
            const float toned = radiance / (1.0F + radiance * options.tonemap_white);
            const int expected =
                (int)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(fminf(toned, 1.0F), 1 / R3D_SKIN_LIGHT_GAMMA));
            colors[mapped] = scene_entity_mesh(scene, entity)->colors[0][0];
            TEST_ASSERT_UINT8_WITHIN(BYTE_TOLERANCE, expected, colors[mapped]);
            release_fixture();
        }
        TEST_ASSERT_GREATER_THAN_INT(MINIMUM_TONEMAP_CHANGE, colors[0] - colors[1]);
    }
}

static void
test_lighting_storage_requires_a_skin_and_light_in_both_orders(void) {
    fixture();
    scene_t* scene = load("test_pair");
    const scene_entity_t entity = scene_find(scene, name_hash("green")), lamp = scene_find(scene, name_hash("camera"));
    static const char* const clips[] = {"rig.move"};
    const size_t before = memory_free_bytes(MEMORY_INTERNAL);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, lamp, 1));
    TEST_ASSERT_NULL(scene->light_transfer);
    TEST_ASSERT_EQUAL_UINT(before, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, "rig", clips, 1, NULL));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, entity, "rig.quad", NULL));
    TEST_ASSERT_NOT_NULL(scene->light_transfer);
    TEST_ASSERT_NOT_NULL(scene->skinned_renderers[entity].light_storage);
    TEST_ASSERT_EQUAL_STRING("green", scene_entity_mesh_id(scene, entity));
    release_fixture();
    skin_fixture_options_t options = SKIN_OPTIONS;
    options.intensity = 0;
    fixture_with_skin(&options);
    scene = load("test_skin");
    TEST_ASSERT_NULL(scene->light_transfer);
    TEST_ASSERT_NULL(scene->skinned_renderers[0].light_storage);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, 0, 1));
    TEST_ASSERT_NOT_NULL(scene->light_transfer);
    TEST_ASSERT_NOT_NULL(scene->skinned_renderers[0].light_storage);
    release_fixture();
    fixture();
    scene = load("test_pair");
    scene->ambient_intensity = SKIN_OPTIONS.intensity;
    const size_t ambient_free = memory_free_bytes(MEMORY_INTERNAL);
    scene_animation_step(scene, RIG_DURATION_MS);
    TEST_ASSERT_NULL(scene->light_transfer);
    TEST_ASSERT_EQUAL_UINT(ambient_free, memory_free_bytes(MEMORY_INTERNAL));
}

static void
test_authored_skin_load_allocation_failures_release_lighting(void) {
    skin_fixture_options_t options = SKIN_OPTIONS;
    options.mode = R3D_SKIN_LIGHT_TABLE;
    fixture_with_skin(&options);
    assert_load_failures_release(&fx.pack, "test_skin");
}

static void
test_authored_light_preserves_static_mesh_colors(void) {
    fixture();
    r3d_lit_mesh_t packed;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(&fx.pack, "red", &packed));
    uint8_t expected[QUAD_VERTICES][R3D_SKIN_LIGHT_CHANNELS];
    memcpy(expected, packed.colors, sizeof expected);
    scene_t* scene = load("test_lit");
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    TEST_ASSERT_EQUAL_INT(1, scene->light_count);
    TEST_ASSERT_NULL(scene->light_transfer);
    assert_light(scene, scene_find(scene, name_hash("sun")), (vec3f_t){0.25F, 0.5F, 0.75F}, 2.0F);
    const size_t available = memory_free_bytes(MEMORY_INTERNAL);
    scene_animation_step(scene, RIG_DURATION_MS);
    TEST_ASSERT_EQUAL_UINT(available, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_EQUAL_PTR(packed.colors, scene_entity_mesh(scene, entity)->colors);
    TEST_ASSERT_EQUAL_MEMORY(expected, scene_entity_mesh(scene, entity)->colors, sizeof expected);
}

static void
test_a_camera_view_stands_at_its_pose_looking_down_the_rotated_plus_z(void) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, (vec3f_t){3.0F, 2.0F, 4.0F});
    transformf_look_at(&pose, (vec3f_t){0.0F, 1.0F, 0.0F}, (vec3f_t){0.0F, 1.0F, 0.0F});
    transformf_set_scale(&pose, (vec3f_t){2.0F, 3.0F, 4.0F});
    const camera_t camera = {.half_fov_short_tan = 1.0F, .near_z = 1.0F};
    const render_view_t view = camera_view(&camera, &pose, (viewport_t){SIZE, SIZE, 0});
    const vec3f_t ahead = quatf_rotate(pose.rotation, (vec3f_t){0.0F, 0.0F, 1.0F});
    TEST_ASSERT_TRUE(vec3f_equal(pose.position, view.position));
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, ahead.x, view.forward.x);
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, ahead.y, view.forward.y);
    TEST_ASSERT_FLOAT_WITHIN(1.0E-6F, ahead.z, view.forward.z);
}

static void
test_translation_only_camera_clip_retains_rotation(void) {
    fixture();
    scene_t* scene = load("test_unturned");
    const scene_entity_t entity = scene_find(scene, name_hash("camera"));
    transformf_t pose = scene_entity_local(scene, entity);
    transformf_set_rotation(&pose, quatf_from_axis_angle((vec3f_t){0, 1, 0}, 0.5F));
    scene_entity_set_local(scene, entity, pose);
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    const transformf_t camera = scene_entity_local(scene, entity);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, camera.position.x);
    TEST_ASSERT_EQUAL_MEMORY(&pose.rotation, &camera.rotation, sizeof pose.rotation);
    const vec3f_t wanted = quatf_rotate(pose.rotation, (vec3f_t){0, 0, 1});
    const vec3f_t forward = quatf_rotate(camera.rotation, (vec3f_t){0, 0, 1});
    TEST_ASSERT_EQUAL_MEMORY(&wanted, &forward, sizeof wanted);
}

static void
test_skin_step_skips_unposed_entities_before_posed_clusters(void) {
    scene_t* scene = rig_fixture("rig.skin4", "rig.quad");
    const scene_entity_t red = scene_find(scene, name_hash("red")), green = scene_find(scene, name_hash("green"));
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, red, SCENE_ANIMATOR_CURRENT, true));
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_FALSE(scene->animators[red].posed);
    TEST_ASSERT_TRUE(scene->animators[green].posed);
    TEST_ASSERT_EQUAL_INT16(-1, scene_entity_mesh(scene, red)->positions[0][0]);
    TEST_ASSERT_EQUAL_INT16(1, scene_entity_mesh(scene, green)->positions[0][0]);
    TEST_ASSERT_EQUAL_INT16(1, scene_entity_mesh(scene, green)->nodes[0].bounds.lo.x);
}

static void
test_failed_second_camera_releases_the_first_animator(void) {
    fixture();
    asset_view_t entry;
    scene_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&fx.pack, "test_aliases", SCENE_ASSET, &entry));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, scene_asset_open(entry, &asset));
    char (*clips)[ASSET_NAME_MAX] = (char (*)[ASSET_NAME_MAX])(uintptr_t)asset.clips;
    memset(clips[1], 0, ASSET_NAME_MAX);
    memcpy(clips[1], "nowhere", sizeof "nowhere");
    const size_t before = memory_free_bytes(MEMORY_PSRAM);
    scene_failure_t why;
    TEST_ASSERT_NULL(scene_load_from(&fx.pack, "test_aliases", &why));
    TEST_ASSERT_EQUAL_STRING("nowhere", why.what);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, why.asset);
    TEST_ASSERT_TRUE(memory_free_bytes(MEMORY_PSRAM) == before);
}

static void
test_replacing_skin_clips_drives_the_new_clip_after_one_step(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_INT16(1, scene_entity_mesh(scene, entity)->positions[0][0]);
    static const char* const clips[] = {"rig.back"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, "rig", clips, 1, NULL));
    TEST_ASSERT_TRUE(scene_animator_play(scene, entity, 0, 0));
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_INT16(-3, scene_entity_mesh(scene, entity)->positions[0][0]);
    static const char* const property[] = {"property.move"};
    scene_failure_t why;
    TEST_ASSERT_FALSE(scene_animator_set_clips(scene, entity, NULL, property, 1, &why));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
    TEST_ASSERT_EQUAL_STRING("rig.back", scene_animator_clip_name(scene, entity, 0));
}

static void
test_disabled_property_animator_does_not_advance_or_write(void) {
    fixture();
    scene_t* scene = load("test_pair");
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    static const char* const clips[] = {"property.move"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, entity, NULL, clips, 1, NULL));
    TEST_ASSERT_TRUE(scene_animator_play(scene, entity, 0, 0));
    scene_entity_set_enabled(scene, entity, false);
    const transformf_t before = scene_entity_local(scene, entity);
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    const transformf_t stored = scene_entity_local(scene, entity);

    TEST_ASSERT_EQUAL_MEMORY(&before, &stored, sizeof before);
    TEST_ASSERT_EQUAL_UINT32(0, scene->animators[entity].layer.current.playhead.t_ms);
}

static void
test_set_transform_defers_placement_until_the_scene_draws(void) {
    fixture();
    scene_t* scene = load("test_pair");
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    const int renderer = scene_renderer_index(scene, entity);
    const mat4f_t before = scene->renderers[renderer].model;
    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, (vec3f_t){2, 3, 4});
    scene_entity_set_local(scene, entity, pose);
    TEST_ASSERT_TRUE(scene->flags[entity] & SCENE_FLAG_DIRTY);
    TEST_ASSERT_EQUAL_MEMORY(&before, &scene->renderers[renderer].model, sizeof before);
    scene->locals[entity].cached = true;
    scene_render(RIG_DURATION_MS / 2, 0, SIZE, SIZE);
    TEST_ASSERT_TRUE(scene->locals[entity].cached);
    TEST_ASSERT_TRUE(scene->flags[entity] & SCENE_FLAG_DIRTY);
    TEST_ASSERT_EQUAL_MEMORY(&before, &scene->renderers[renderer].model, sizeof before);
    TEST_ASSERT_TRUE(scene_activate(scene, SCENE_CAMERA_FIRST));
    frame(0);
    const mat4f_t wanted = transformf_compute_matrix(&pose);
    TEST_ASSERT_EQUAL_MEMORY(&wanted, &scene->renderers[renderer].model, sizeof wanted);
    TEST_ASSERT_FALSE(scene->flags[entity] & SCENE_FLAG_DIRTY);
}

static void
test_skeleton_animator_count_tracks_attachment_replacement_and_release(void) {
    fixture();
    scene_t* scene = load("test_pair");
    const scene_entity_t red = scene_find(scene, name_hash("red")), green = scene_find(scene, name_hash("green"));
    TEST_ASSERT_EQUAL_INT(0, scene->skeleton_animator_count);
    TEST_ASSERT_EQUAL_INT(0, scene->animator_count);
    static const char* const clips[] = {"rig.move"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, red, "rig", clips, 1, NULL));
    TEST_ASSERT_EQUAL_INT(1, scene->skeleton_animator_count);
    TEST_ASSERT_EQUAL_INT(1, scene->animator_count);
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, red, "rig", clips, 1, NULL));
    TEST_ASSERT_EQUAL_INT(1, scene->skeleton_animator_count);
    TEST_ASSERT_EQUAL_INT(1, scene->animator_count);
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, green, "rig", clips, 1, NULL));
    TEST_ASSERT_EQUAL_INT(2, scene->skeleton_animator_count);
    TEST_ASSERT_EQUAL_INT(2, scene->animator_count);
    static const char* const property[] = {"property.move"};
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, red, NULL, property, 1, NULL));
    TEST_ASSERT_EQUAL_INT(1, scene->skeleton_animator_count);
    TEST_ASSERT_EQUAL_INT(2, scene->animator_count);
    scene_animation_release(scene);
    TEST_ASSERT_EQUAL_INT(0, scene->skeleton_animator_count);
    TEST_ASSERT_EQUAL_INT(0, scene->animator_count);
}

#ifdef HOST_HEAP_ARENA
static void
test_scene_without_animators_does_not_access_components(void) {
    fixture();
    scene_t untouched = *load("test_pair");
    untouched.animators = NULL;
    untouched.skinned_renderers = NULL;
    scene_animation_step(&untouched, RIG_DURATION_MS / 2);
}

static void
test_static_scene_and_unposed_skin_do_not_dispatch_jobs(void) {
    fixture();
    scene_t* scene = load("test_pair");
    const int before = job_calls_count();
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_INT(before, job_calls_count());
    release_fixture();
    scene = rig_fixture("rig.skin2", NULL);
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(
        scene_animator_set_paused(scene, scene_find(scene, name_hash("red")), SCENE_ANIMATOR_CURRENT, true));
    const int paused = job_calls_count();
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_INT(paused + 1, job_calls_count());
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[scene_find(scene, name_hash("red"))];
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    for (int entities = 1; entities <= scene->asset.entity_count; entities++) {
        for (int clusters = 1; clusters <= skin->bind.cluster_count; clusters++) {
            scene_t limited = *scene;
            limited.asset.entity_count = entities;
            if (entities == 1) {
                limited.animators += entity;
                limited.skinned_renderers += entity;
                limited.flags += entity;
            }
            const int saved = skin->bind.cluster_count;
            skin->bind.cluster_count = clusters;
            TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, false));
            scene->animators[entity].dirty = true;
            const int calls = job_calls_count();
            scene_animation_step(&limited, 0);
            TEST_ASSERT_EQUAL_INT(calls + (entities > 1) + (clusters > 1), job_calls_count());
            TEST_ASSERT_TRUE(scene->animators[entity].posed);
            TEST_ASSERT_EQUAL_INT16(skin->bind.positions[0][0], skin->output.positions[0][0]);
            skin->bind.cluster_count = saved;
        }
    }
}
#endif

static void
test_disabled_light_skin_and_two_object_rotations(void) {
    scene_t* scene = rig_fixture("rig.skin2", "rig.quad");
    const scene_entity_t red = scene_find(scene, name_hash("red")), green = scene_find(scene, name_hash("green"));
    const scene_entity_t light = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    transformf_t transform = scene_entity_local(scene, green);
    transform.rotation = quatf_from_euler((vec3f_t){MATH_PI / 2, 0, 0});
    scene_entity_set_local(scene, green, transform);
    scene_animation_step(scene, 0);
    const scene_skinned_renderer_t* a = &scene->skinned_renderers[red];
    const scene_skinned_renderer_t* b = &scene->skinned_renderers[green];
    TEST_ASSERT_FLOAT_WITHIN(0.0001F, SCENE_LIGHT_AXIS.y, scene->world_lights.direction[0].y);
    TEST_ASSERT_FLOAT_WITHIN(0.0001F, quatf_rotate(transform.rotation, SCENE_LIGHT_AXIS).z,
                             b->normal_palette[0].m[2][1]);
    scene_entity_set_enabled(scene, light, false);
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_INT(0, scene->world_lights.count);
    for (int v = 0; v < a->bind.vertex_count; v++) {
        for (int c = 0; c < R3D_SKIN_LIGHT_CHANNELS; c++) {
            TEST_ASSERT_EQUAL_UINT8(0, a->colors[v][c]);
        }
    }
    scene->ambient_color = SCENE_LIGHT_COLOR_DEFAULT;
    scene->ambient_intensity = 1;
    scene->flags[light] |= SCENE_FLAG_SKIN_DIRTY;
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_MEMORY(a->bind.colors, a->colors, (size_t)a->bind.vertex_count * R3D_SKIN_LIGHT_CHANNELS);
    scene_entity_set_enabled(scene, red, false);
    scene_entity_set_enabled(scene, light, true);
    scene_animation_step(scene, 0);
    TEST_ASSERT_FALSE(scene->animators[red].posed);
    TEST_ASSERT_EQUAL_PTR(a->colors, scene_entity_mesh(scene, red)->colors);
    TEST_ASSERT_EQUAL_INT(1, scene->world_lights.count);
    release_fixture();
}

static void
test_light_transfer_allocation_refuses_without_changes(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t lamp = scene_find(scene, name_hash("camera"));
    scene_fail_allocation_after = 0;
    TEST_ASSERT_FALSE(scene_light_set_intensity(scene, lamp, 1));
    TEST_ASSERT_NULL(scene->light_transfer);
    TEST_ASSERT_EQUAL_INT(0, scene->light_count);
    TEST_ASSERT_EQUAL_INT(SCENE_LIGHT_NONE, scene->lights[lamp].kind);
    scene_fail_allocation_after = -1;
}

static void
test_idle_lit_skin_skips_and_transform_changes_run(void) {
    scene_fail_allocation_after = -1;
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red"));
    const scene_entity_t light = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    scene_t* active_scene = show("test_sky", SCENE_CAMERA_FIRST);
    TEST_ASSERT_NOT_EQUAL(scene, active_scene);
    scene_render(0, 0, SIZE, SIZE);
    for (int frame = 0; frame < 3; frame++) {
        scene_render(0, 0, SIZE, SIZE);
        TEST_ASSERT_FALSE(scene->animators[entity].posed);
    }
    TEST_ASSERT_TRUE((scene->flags[entity] & SCENE_FLAG_DIRTY) != 0);
    transformf_t pose = scene_entity_local(scene, light);
    pose.rotation = quatf_from_euler((vec3f_t){MATH_PI, 0, 0});
    scene_entity_set_local(scene, light, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    scene->flags[light] &= (uint8_t)~SCENE_FLAG_DIRTY;
    scene_entity_set_local(scene, entity, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    release_fixture();
}

static void
test_invalid_normal_transform_skips_lighting_and_recovers(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), light = scene_find(scene, name_hash("camera"));
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    scene->ambient_color = SCENE_LIGHT_COLOR_DEFAULT;
    scene->ambient_intensity = 1;
    static const float scales[] = {0, NAN, INFINITY};
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        transformf_t pose = scene_entity_local(scene, entity);
        transformf_set_scale(&pose, (vec3f_t){scales[i], 1, 1});
        scene_entity_set_local(scene, entity, pose);
        scene_animation_step(scene, 0);
        TEST_ASSERT_FALSE(skin->lighting_valid);
        TEST_ASSERT_EQUAL_PTR(skin->colors, scene_entity_mesh(scene, entity)->colors);
        for (int v = 0; v < skin->bind.vertex_count; v++) {
            for (int c = 0; c < R3D_SKIN_LIGHT_CHANNELS; c++) {
                TEST_ASSERT_EQUAL_UINT8(0, skin->colors[v][c]);
            }
        }
    }
    transformf_t pose = scene_entity_local(scene, entity);
    transformf_set_scale(&pose, (vec3f_t){1, 1, 1});
    scene_entity_set_local(scene, entity, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(skin->lighting_valid);
    TEST_ASSERT_GREATER_THAN_UINT8(0, skin->colors[0][0]);
}

static void
test_lit_load_unload_releases_internal_storage(void) {
    const size_t before = memory_free_bytes(MEMORY_INTERNAL);
    scene_t* scene = rig_fixture("rig.skin2", "rig.quad");
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, scene_find(scene, name_hash("camera")), 1));
    TEST_ASSERT_LESS_THAN_UINT(before, memory_free_bytes(MEMORY_INTERNAL));
    const scene_entity_t red = scene_find(scene, name_hash("red"));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, red, "rig.skin4", NULL));
    TEST_ASSERT_LESS_THAN_UINT(before, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, red, "rig.skin2", NULL));
    release_fixture();
    TEST_ASSERT_EQUAL_UINT(before, memory_free_bytes(MEMORY_INTERNAL));
}

static void
test_first_light_storage_rollback_and_lit_skin_allocation_failure(void) {
    scene_t* scene = rig_fixture("rig.skin2", "rig.quad");
    const scene_entity_t red = scene_find(scene, name_hash("red")), green = scene_find(scene, name_hash("green"));
    const scene_entity_t light = scene_find(scene, name_hash("camera"));
    scene_skinned_renderer_t* a = &scene->skinned_renderers[red];
    scene_skinned_renderer_t* b = &scene->skinned_renderers[green];
    const size_t free_before = memory_free_bytes(MEMORY_INTERNAL);
    for (int allocation = 0; allocation < 3; allocation++) {
        const scene_t saved = *scene;
        const uint8_t flags = scene->flags[light];
        scene_fail_allocation_after = allocation;
        TEST_ASSERT_FALSE(scene_light_set_intensity(scene, light, 1));
        TEST_ASSERT_EQUAL_MEMORY(&saved, scene, sizeof saved);
        TEST_ASSERT_EQUAL_UINT8(flags, scene->flags[light]);
        TEST_ASSERT_EQUAL_INT(SCENE_LIGHT_NONE, scene->lights[light].kind);
        TEST_ASSERT_NULL(a->light_storage);
        TEST_ASSERT_NULL(b->light_storage);
        TEST_ASSERT_NULL(a->colors);
        TEST_ASSERT_NULL(b->colors);
        TEST_ASSERT_NULL(a->normal_palette);
        TEST_ASSERT_NULL(b->normal_palette);
        TEST_ASSERT_EQUAL_PTR(a->bind.colors, scene_entity_mesh(scene, red)->colors);
        TEST_ASSERT_EQUAL_PTR(b->bind.colors, scene_entity_mesh(scene, green)->colors);
        TEST_ASSERT_EQUAL_UINT(free_before, memory_free_bytes(MEMORY_INTERNAL));
    }
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    const void* storage = a->internal_storage;
    const void* light_storage = a->light_storage;
    const void* colors = scene_entity_mesh(scene, red)->colors;
    const size_t lit_free = memory_free_bytes(MEMORY_INTERNAL);
    scene_fail_allocation_after = 0;
    scene_failure_t why;
    TEST_ASSERT_FALSE(scene_renderer_set_skin(scene, red, "rig.skin4", &why));
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_MEMORY, why.status);
    TEST_ASSERT_EQUAL_PTR(storage, a->internal_storage);
    TEST_ASSERT_EQUAL_PTR(light_storage, a->light_storage);
    TEST_ASSERT_EQUAL_PTR(colors, scene_entity_mesh(scene, red)->colors);
    TEST_ASSERT_EQUAL_UINT(lit_free, memory_free_bytes(MEMORY_INTERNAL));
}

static void
test_paused_skin_setters_replacement_and_reenable(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), light = scene_find(scene, name_hash("camera"));
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    TEST_ASSERT_NULL(skin->colors);
    TEST_ASSERT_NULL(skin->normal_palette);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    transformf_t pose = scene_entity_local(scene, light);
    pose.rotation = quatf_from_euler((vec3f_t){MATH_PI / 2, 0, 0});
    scene_entity_set_local(scene, light, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_UINT8_WITHIN(1, skin->bind.colors[0][0], skin->colors[0][0]);
    scene_animation_step(scene, 0);
    TEST_ASSERT_FALSE(scene->animators[entity].posed);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 0));
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_EQUAL_UINT8(0, skin->colors[0][0]);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene_light_set_color(scene, light, (vec3f_t){0, 1, 1}));
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_EQUAL_UINT8(0, skin->colors[0][0]);
    TEST_ASSERT_TRUE(scene_light_set_color(scene, light, SCENE_LIGHT_COLOR_DEFAULT));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, entity, "rig.skin4", NULL));
    TEST_ASSERT_EQUAL_PTR(skin->colors, scene_entity_mesh(scene, entity)->colors);
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_GREATER_THAN_UINT8(0, skin->bind.colors[0][0]);
    TEST_ASSERT_UINT8_WITHIN(1, skin->bind.colors[0][0], skin->colors[0][0]);
    scene_entity_set_enabled(scene, entity, false);
    TEST_ASSERT_TRUE(scene_light_set_color(scene, light, SCENE_LIGHT_COLOR_DEFAULT));
    scene_animation_step(scene, 0);
    scene_entity_set_enabled(scene, entity, true);
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_UINT8_WITHIN(1, skin->bind.colors[0][0], skin->colors[0][0]);
}

static void
test_runtime_scale_and_reflection_match_float_reference(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), light = scene_find(scene, name_hash("camera"));
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, entity, SCENE_SKIN_LIGHT_RENORMALISED));
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    transformf_t lamp = scene_entity_local(scene, light);
    lamp.rotation = quatf_from_euler((vec3f_t){MATH_PI / 2, 0, 0});
    scene_entity_set_local(scene, light, lamp);
    uint8_t* record = (uint8_t*)(uintptr_t)skin->skin.vertices;
    for (int v = 0; v < skin->bind.vertex_count; v++) {
        int8_t* normal = (int8_t*)(record + v * skin->skin.vertex_stride + 2 * skin->skin.influences);
        normal[0] = normal[2] = R3D_SKIN_NORMAL_SCALE;
        normal[1] = 0;
    }
    static const float scales[] = {2, -2};
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        transformf_t pose = scene_entity_local(scene, entity);
        transformf_set_scale(&pose, (vec3f_t){scales[i], 1, 1});
        scene_entity_set_local(scene, entity, pose);
        scene_animation_step(scene, 0);
        TEST_ASSERT_TRUE(scene->animators[entity].posed);
        const vec3f_t normal = {1 / scales[i], 0, 1};
        const float facing = normal.z / sqrtf(vec3f_dot(normal, normal));
        const float albedo = powf(skin->bind.colors[0][0] / (float)R3D_SKIN_LIGHT_BYTE_SCALE, R3D_SKIN_LIGHT_GAMMA);
        const int expected = (int)lrintf(R3D_SKIN_LIGHT_BYTE_SCALE * powf(albedo * facing, 1 / R3D_SKIN_LIGHT_GAMMA));
        TEST_ASSERT_UINT8_WITHIN(2, expected, skin->colors[0][0]);
    }
}

static void
test_property_transform_relights_paused_skin(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), light = scene_find(scene, name_hash("camera"));
    static const char* const clips[] = {"property.move"};
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    TEST_ASSERT_TRUE(scene_animator_set_clips(scene, light, NULL, clips, 1, NULL));
    TEST_ASSERT_TRUE(scene_animator_play(scene, light, 0, 0));
    scene_animation_step(scene, 0);
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_EQUAL_FLOAT(RIG_TRAVEL / 2, scene->locals[entity].position.x);
}

static void
test_face_color_skin_refuses_lights_and_lit_attachment(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), light = scene_find(scene, name_hash("camera"));
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    const uint8_t (*colors)[R3D_SKIN_LIGHT_CHANNELS] = skin->bind.colors;
    skin->bind.colors = NULL;
    static const uint16_t face_colors[] = {0};
    skin->bind.face_colors = face_colors;
    TEST_ASSERT_FALSE(scene_light_set_intensity(scene, light, 1));
    TEST_ASSERT_FALSE(scene_light_set_color(scene, light, SCENE_LIGHT_COLOR_DEFAULT));
    TEST_ASSERT_EQUAL_INT(0, scene->light_count);
    TEST_ASSERT_NULL(scene->light_transfer);
    skin->bind.colors = colors;
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
    const void* storage = skin->internal_storage;
    skin->bind.colors = NULL;
    scene_failure_t why;
    TEST_ASSERT_FALSE(scene_renderer_set_skin(scene, entity, "rig.skin4", &why));
    TEST_ASSERT_EQUAL_PTR(storage, skin->internal_storage);
    TEST_ASSERT_EQUAL_INT(SCENE_ERR_ASSET, why.status);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, why.asset);
    TEST_ASSERT_EQUAL_STRING(SCENE_LIGHT_FAILURE_ALBEDO, why.what);
    skin->bind.colors = colors;
}

static void
test_light_attachment_defaults_refusals_and_capacity(void) {
    enum { ENTITIES = R3D_SKIN_LIGHT_MAX + 1 };

    scene_light_t lights[ENTITIES] = {0};
    uint8_t flags[ENTITIES] = {0};
    uint16_t roots[ENTITIES];
    memset(roots, 0xFF, sizeof roots);
    scene_t scene = {.asset = {.entity_count = ENTITIES, .parents = roots}, .lights = lights, .flags = flags};
    TEST_ASSERT_TRUE(scene_light_set_color(&scene, 0, (vec3f_t){0.5F, 0.25F, 0.125F}));
    TEST_ASSERT_EQUAL_FLOAT(SCENE_LIGHT_INTENSITY_DEFAULT, scene_light_intensity(&scene, 0));
    TEST_ASSERT_TRUE(scene_light_set_intensity(&scene, 1, 2.0F));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, scene_light_color(&scene, 1).x);
    TEST_ASSERT_EQUAL_FLOAT(0, scene_light_color(&scene, 2).x);
    static const vec3f_t invalid[] = {{-0.1F, 0, 0}, {0, 1.1F, 0}, {0, 0, NAN}, {INFINITY, 0, 0}};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        TEST_ASSERT_FALSE(scene_light_set_color(&scene, 0, invalid[i]));
        TEST_ASSERT_FALSE(scene_light_set_color(&scene, 2, invalid[i]));
        TEST_ASSERT_EQUAL_FLOAT(0.5F, scene_light_color(&scene, 0).x);
        TEST_ASSERT_EQUAL_INT(2, scene.light_count);
    }
    static const float invalid_intensity[] = {-1, NAN, INFINITY};
    for (size_t i = 0; i < sizeof invalid_intensity / sizeof invalid_intensity[0]; i++) {
        TEST_ASSERT_FALSE(scene_light_set_intensity(&scene, 1, invalid_intensity[i]));
        TEST_ASSERT_EQUAL_FLOAT(2, scene_light_intensity(&scene, 1));
    }
    TEST_ASSERT_FALSE(scene_light_set_intensity(&scene, SCENE_ENTITY_NONE, 1));
    TEST_ASSERT_FALSE(scene_light_set_color(&scene, SCENE_ENTITY_NONE, SCENE_LIGHT_COLOR_DEFAULT));
    for (int i = 2; i < R3D_SKIN_LIGHT_MAX; i++) {
        TEST_ASSERT_TRUE(scene_light_set_intensity(&scene, (scene_entity_t)i, 1));
    }
    TEST_ASSERT_FALSE(scene_light_set_intensity(&scene, R3D_SKIN_LIGHT_MAX, 1));
    TEST_ASSERT_FALSE(scene_light_set_color(&scene, R3D_SKIN_LIGHT_MAX, SCENE_LIGHT_COLOR_DEFAULT));
    TEST_ASSERT_EQUAL_INT(SCENE_LIGHT_NONE, lights[R3D_SKIN_LIGHT_MAX].kind);
    TEST_ASSERT_TRUE(scene_light_set_intensity(&scene, 0, 3));
    TEST_ASSERT_TRUE(scene_light_set_intensity(&scene, 0, 1e-38F));
    TEST_ASSERT_TRUE(scene_light_set_intensity(&scene, 0, 3e38F));
    memory_free(scene.light_transfer);
}

static void
test_scene_light_rotation_shades_only_skin_and_matches_kernel(void) {
    static const char* const skins[] = {"rig.skin2", "rig.skin4"};

    enum { DEGREES_HALF_TURN = 180 };

    const vec3f_t sun_angles = {40.0F * MATH_PI / DEGREES_HALF_TURN, 30.0F * MATH_PI / DEGREES_HALF_TURN, 0};
    for (size_t i = 0; i < sizeof skins / sizeof skins[0]; i++) {
        scene_t* scene = rig_fixture(skins[i], NULL);
        const scene_entity_t entity = scene_find(scene, name_hash("red"));
        const scene_entity_t light = scene_find(scene, name_hash("camera"));
        const scene_entity_t baked = scene_find(scene, name_hash("green"));
        scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
        const r3d_lit_mesh_t* mesh = scene_entity_mesh(scene, entity);
        const r3d_lit_mesh_t* static_mesh = scene_entity_mesh(scene, baked);
        uint8_t baked_colors[QUAD_VERTICES][3];
        memcpy(baked_colors, static_mesh->colors, sizeof baked_colors);
        TEST_ASSERT_EQUAL_PTR(skin->bind.colors, mesh->colors);
        TEST_ASSERT_EQUAL_FLOAT(SCENE_TONEMAP_WHITE_NONE, scene->tonemap_white);
        TEST_ASSERT_TRUE(scene_light_set_intensity(scene, light, 1));
        transformf_t pose = scene_entity_local(scene, light);
        pose.rotation = quatf_from_euler(sun_angles);
        scene_entity_set_local(scene, light, pose);
        scene_animation_step(scene, 0);
        TEST_ASSERT_FLOAT_WITHIN(0.01F, quatf_rotate(pose.rotation, SCENE_LIGHT_AXIS).x,
                                 scene->world_lights.direction[0].x);
        TEST_ASSERT_FLOAT_WITHIN(0.01F, quatf_rotate(pose.rotation, SCENE_LIGHT_AXIS).y,
                                 scene->world_lights.direction[0].y);
        TEST_ASSERT_FLOAT_WITHIN(0.01F, quatf_rotate(pose.rotation, SCENE_LIGHT_AXIS).z,
                                 scene->world_lights.direction[0].z);
        TEST_ASSERT_EQUAL_PTR(skin->colors, mesh->colors);
        TEST_ASSERT_EQUAL_MEMORY(baked_colors, static_mesh->colors, sizeof baked_colors);
        uint8_t lit_colors[RIG_VERTICES][3];
        memcpy(lit_colors, mesh->colors, sizeof lit_colors);
        const size_t bytes = r3d_skin_output_bytes(&skin->bind);
        void* block = malloc(bytes);
        TEST_ASSERT_NOT_NULL(block);
        r3d_skin_output_t output;
        TEST_ASSERT_TRUE(r3d_skin_output_init(&skin->bind, block, bytes, &output));
        uint8_t expected[RIG_VERTICES][3];
        r3d_skin_clusters_lit(&skin->skin, &skin->bind, skin->palette, 0, skin->bind.cluster_count, &output,
                              skin->normal_palette, &scene->world_lights, expected);
        TEST_ASSERT_EQUAL_MEMORY(expected, mesh->colors, sizeof expected);
        free(block);
        TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
        pose.rotation = quatf_from_euler((vec3f_t){MATH_PI, 0, 0});
        scene_entity_set_local(scene, light, pose);
        scene_animation_step(scene, 0);
        TEST_ASSERT_NOT_EQUAL(0, memcmp(lit_colors, mesh->colors, sizeof lit_colors));
        pose.rotation = quatf_identity();
        scene_entity_set_local(scene, light, pose);
        scene_animation_step(scene, 0);
        memcpy(lit_colors, mesh->colors, sizeof lit_colors);
        const quatf_t turn = quatf_from_euler(sun_angles);
        pose.rotation = turn;
        scene_entity_set_local(scene, light, pose);
        transformf_t renderer = scene_entity_local(scene, entity);
        renderer.rotation = turn;
        scene_entity_set_local(scene, entity, renderer);
        scene_animation_step(scene, 0);
        TEST_ASSERT_EQUAL_MEMORY(lit_colors, mesh->colors, sizeof lit_colors);
        release_fixture();
    }
}

static void
test_skin_modes_share_table_transactionally_and_rebuild_only_for_lights(void) {
    const size_t before = memory_free_bytes(MEMORY_INTERNAL);
    scene_t* scene = rig_fixture("rig.skin2", "rig.quad");
    const scene_entity_t red = scene_find(scene, name_hash("red")), green = scene_find(scene, name_hash("green")),
                         lamp = scene_find(scene, name_hash("camera"));
    scene_skinned_renderer_t* a = &scene->skinned_renderers[red];
    TEST_ASSERT_EQUAL_INT(SCENE_SKIN_LIGHT_DIRECT, a->light_mode);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, lamp, 1));
    scene_animation_step(scene, 0);
    TEST_ASSERT_NULL(scene->light_table);
    const size_t lit_free = memory_free_bytes(MEMORY_INTERNAL);
    const void* tail = a->light_storage;
    scene_fail_allocation_after = 0;
    TEST_ASSERT_FALSE(scene_skin_light_set_mode(scene, red, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_EQUAL_INT(SCENE_SKIN_LIGHT_DIRECT, a->light_mode);
    TEST_ASSERT_NULL(scene->light_table);
    TEST_ASSERT_EQUAL_UINT(lit_free, memory_free_bytes(MEMORY_INTERNAL));
    scene_fail_allocation_after = -1;
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, red, SCENE_SKIN_LIGHT_RENORMALISED));
    TEST_ASSERT_EQUAL_PTR(tail, a->light_storage);
    TEST_ASSERT_EQUAL_UINT(lit_free, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, red, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(lit_free - sizeof *scene->light_table, memory_free_bytes(MEMORY_INTERNAL));
    scene_animation_step(scene, 0);
    TEST_ASSERT_FALSE(scene->light_table_dirty);
    const uint32_t cell = scene->light_table->cells[0];
    scene->light_table->cells[0] ^= 1U;
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_UINT32(cell ^ 1U, scene->light_table->cells[0]);
    transformf_t pose = scene_entity_local(scene, red);
    pose.rotation = quatf_from_euler((vec3f_t){0, 0, MATH_PI / 2});
    scene_entity_set_local(scene, red, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_UINT32(cell ^ 1U, scene->light_table->cells[0]);
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, lamp, 1));
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_UINT32(cell, scene->light_table->cells[0]);
    scene->light_table->cells[0] ^= 1U;
    pose = scene_entity_local(scene, lamp);
    pose.rotation = quatf_from_euler((vec3f_t){0, 0, MATH_PI / 2});
    scene_entity_set_local(scene, lamp, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_FALSE(scene->light_table_dirty);
    TEST_ASSERT_NOT_EQUAL(cell ^ 1U, scene->light_table->cells[0]);
    const void* shared = scene->light_table;
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, green, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_EQUAL_PTR(shared, scene->light_table);
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, red, SCENE_SKIN_LIGHT_DIRECT));
    TEST_ASSERT_EQUAL_PTR(shared, scene->light_table);
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, green, SCENE_SKIN_LIGHT_RENORMALISED));
    TEST_ASSERT_NULL(scene->light_table);
    TEST_ASSERT_EQUAL_UINT(lit_free, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, green, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_TRUE(scene_renderer_set_skin(scene, green, "rig.quad", NULL));
    TEST_ASSERT_NULL(scene->light_table);
    release_fixture();
    TEST_ASSERT_EQUAL_UINT(before, memory_free_bytes(MEMORY_INTERNAL));
}

static void
test_table_later_frames_mode_changes_and_light_enable(void) {
    scene_t* scene = rig_fixture("rig.skin2", NULL);
    const scene_entity_t entity = scene_find(scene, name_hash("red")), lamp = scene_find(scene, name_hash("camera"));
    scene_skinned_renderer_t* skin = &scene->skinned_renderers[entity];
    TEST_ASSERT_FALSE(scene_skin_light_set_mode(NULL, entity, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_FALSE(scene_skin_light_set_mode(scene, SCENE_ENTITY_NONE, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_FALSE(scene_skin_light_set_mode(scene, lamp, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_FALSE(scene_skin_light_set_mode(scene, entity, (scene_skin_light_mode_t)-1));
    TEST_ASSERT_FALSE(scene_skin_light_set_mode(scene, entity, (scene_skin_light_mode_t)(SCENE_SKIN_LIGHT_TABLE + 1)));
    TEST_ASSERT_TRUE(scene_light_set_intensity(scene, lamp, 1));
    transformf_t light_pose = scene_entity_local(scene, lamp);
    light_pose.rotation = quatf_from_euler((vec3f_t){MATH_PI / 2, 0, 0});
    scene_entity_set_local(scene, lamp, light_pose);
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, entity, SCENE_SKIN_LIGHT_TABLE));
    scene_animation_step(scene, 0);
    uint8_t built[RIG_VERTICES][R3D_SKIN_LIGHT_CHANNELS];
    memcpy(built, skin->colors, sizeof built);
    TEST_ASSERT_GREATER_THAN_UINT8(0, built[0][0]);
    const float scale = scene->world_lights.table_scale;
    const void* table = scene->light_table;
    const int users = scene->light_table_users;
    const size_t available = memory_free_bytes(MEMORY_INTERNAL);
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, entity, SCENE_SKIN_LIGHT_TABLE));
    TEST_ASSERT_EQUAL_PTR(table, scene->light_table);
    TEST_ASSERT_EQUAL_INT(users, scene->light_table_users);
    TEST_ASSERT_EQUAL_UINT(available, memory_free_bytes(MEMORY_INTERNAL));
    TEST_ASSERT_FALSE(scene->flags[entity] & SCENE_FLAG_SKIN_DIRTY);
    scene_animation_step(scene, RIG_DURATION_MS / 2);
    TEST_ASSERT_EQUAL_MEMORY(built, skin->colors, sizeof built);
    TEST_ASSERT_EQUAL_FLOAT(scale, scene->world_lights.table_scale);
    transformf_t pose = scene_entity_local(scene, entity);
    transformf_set_position(&pose, (vec3f_t){1, 2, 3});
    scene_entity_set_local(scene, entity, pose);
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_MEMORY(built, skin->colors, sizeof built);
    TEST_ASSERT_GREATER_THAN_UINT8(0, skin->colors[0][0]);
    TEST_ASSERT_TRUE(scene_animator_set_paused(scene, entity, SCENE_ANIMATOR_CURRENT, true));
    transformf_set_scale(&pose, (vec3f_t){1, 1, 2});
    scene_entity_set_local(scene, entity, pose);
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, entity, SCENE_SKIN_LIGHT_DIRECT));
    scene_animation_step(scene, 0);
    const uint8_t direct = skin->colors[0][0];
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, entity, SCENE_SKIN_LIGHT_RENORMALISED));
    scene_animation_step(scene, 0);
    TEST_ASSERT_TRUE(scene->animators[entity].posed);
    TEST_ASSERT_GREATER_THAN_UINT8(direct, skin->colors[0][0]);
    TEST_ASSERT_TRUE(scene_skin_light_set_mode(scene, entity, SCENE_SKIN_LIGHT_TABLE));
    scene_animation_step(scene, 0);
    scene_entity_set_enabled(scene, lamp, false);
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_UINT8(0, skin->colors[0][0]);
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        TEST_ASSERT_EQUAL_UINT32(0, scene->light_table->cells[cell]);
    }
    scene_entity_set_enabled(scene, lamp, true);
    scene_animation_step(scene, 0);
    TEST_ASSERT_EQUAL_MEMORY(built, skin->colors, sizeof built);
    release_fixture();
}

void
run_scene_suite(void) {
    /* The tests count the heap, so they run on it; the board lends the app
     * arena, put back once they are done. */
    scene_unload_all();
    const memory_take_fn lender = scene_set_take(NULL);
    RUN_TEST(test_table_later_frames_mode_changes_and_light_enable);
    RUN_TEST(test_skin_modes_share_table_transactionally_and_rebuild_only_for_lights);
    RUN_TEST(test_invalid_normal_transform_skips_lighting_and_recovers);
    RUN_TEST(test_lit_load_unload_releases_internal_storage);
    RUN_TEST(test_first_light_storage_rollback_and_lit_skin_allocation_failure);
    RUN_TEST(test_face_color_skin_refuses_lights_and_lit_attachment);
    RUN_TEST(test_paused_skin_setters_replacement_and_reenable);
    RUN_TEST(test_runtime_scale_and_reflection_match_float_reference);
    RUN_TEST(test_property_transform_relights_paused_skin);
    RUN_TEST(test_disabled_light_skin_and_two_object_rotations);
    RUN_TEST(test_light_transfer_allocation_refuses_without_changes);
    RUN_TEST(test_idle_lit_skin_skips_and_transform_changes_run);
    RUN_TEST(test_light_attachment_defaults_refusals_and_capacity);
    RUN_TEST(test_scene_light_rotation_shades_only_skin_and_matches_kernel);
    RUN_TEST(test_authored_skin_modes_animation_and_ambient);
    RUN_TEST(test_authored_light_preserves_static_mesh_colors);
    RUN_TEST(test_authored_tonemap_changes_skin_colors_in_both_attachment_orders);
    RUN_TEST(test_lighting_storage_requires_a_skin_and_light_in_both_orders);
    RUN_TEST(test_authored_skin_load_allocation_failures_release_lighting);
    RUN_TEST(test_translation_only_camera_clip_retains_rotation);
    RUN_TEST(test_a_camera_view_stands_at_its_pose_looking_down_the_rotated_plus_z);
    RUN_TEST(test_skin_step_skips_unposed_entities_before_posed_clusters);
    RUN_TEST(test_failed_second_camera_releases_the_first_animator);
    RUN_TEST(test_replacing_skin_clips_drives_the_new_clip_after_one_step);
    RUN_TEST(test_disabled_property_animator_does_not_advance_or_write);
    RUN_TEST(test_set_transform_defers_placement_until_the_scene_draws);
    RUN_TEST(test_skeleton_animator_count_tracks_attachment_replacement_and_release);
#ifdef HOST_HEAP_ARENA
    RUN_TEST(test_scene_without_animators_does_not_access_components);
    RUN_TEST(test_static_scene_and_unposed_skin_do_not_dispatch_jobs);
#endif

    RUN_TEST(test_mesh_reader_refuses_overlapping_cluster_vertices);
    RUN_TEST(test_mesh_reader_requires_complete_cluster_coverage);
    RUN_TEST(test_mesh_reader_accepts_only_positive_int_scales);
    RUN_TEST(test_clip_bind_failure_names_the_first_rejected_clip);
    RUN_TEST(test_skinned_scene_draws_clip_output_and_reskins_from_bind);
    RUN_TEST(test_skin_split_inside_an_entity_matches_full_range);
    RUN_TEST(test_attachment_count_mismatches_preserve_previous_components);
    RUN_TEST(test_scene_open_refuses_shear_and_reflection_naming_the_entity);
    RUN_TEST(test_property_animation_moves_a_drawn_entity);
    RUN_TEST(test_paused_camera_seek_wraps_or_clamps_and_invalidates_the_entity_matrix);
    RUN_TEST(test_inactive_property_target_invalidates_its_entity_matrix);
    RUN_TEST(test_unchanged_poses_skip_skinning_and_refit);
    RUN_TEST(test_previous_slot_pause_and_empty_or_frozen_controls);
    RUN_TEST(test_animator_queries_and_controls_refuse_missing_or_out_of_range);
    RUN_TEST(test_set_transform_reads_back_exactly_and_drives_placement);
#ifndef DEVICE_BUILD
    RUN_TEST(test_shell_expanded_pass_replays_build_mark_without_raw_writes);
    RUN_TEST(test_paused_or_empty_expanded_scene_retains_its_picture);
    RUN_TEST(test_expanded_scene_in_separate_picture_matches_upscale);
    RUN_TEST(test_expanded_frame_lifecycle_and_raw_draw_guard);
    RUN_TEST(test_blended_expand_blends_within_seams_and_doubles_across_them);
    RUN_TEST(test_expanded_overlay_resets_and_restores_clip);
    RUN_TEST(test_ui_replay_over_expanded_scene_matches_full_frame);
    RUN_TEST(test_camera_present_composes_half_picture_at_any_height);
    RUN_TEST(test_camera_present_scanline_and_double_compose_both_quarters);
    RUN_TEST(test_camera_present_default_setter_and_dynamic_size);
    RUN_TEST(test_camera_small_fill_default_setter_and_view);
    RUN_TEST(test_camera_small_fill_changes_scene_pixels);
    RUN_TEST(test_non_half_compose_upscales_to_the_full_destination);
    RUN_TEST(test_failed_half_allocation_keeps_full_upscale_unexpanded);
    RUN_TEST(test_expanded_frame_without_ui_does_not_replay_the_previous_hud);
#endif
    RUN_TEST(test_a_scene_loads_by_name_and_unloading_gives_back_everything_it_took);
    RUN_TEST(test_a_lent_scene_takes_from_its_lender_and_unloading_returns_it_to_the_lender);
    RUN_TEST(test_a_refused_take_loads_nothing_and_a_refused_scratch_draws_nothing);
    RUN_TEST(test_a_camera_clears_to_the_colour_its_scene_gives);
    RUN_TEST(test_a_load_that_fails_says_what_it_was_about);
    RUN_TEST(test_a_success_clears_what_an_earlier_failure_said);
    RUN_TEST(test_a_malformed_entry_a_missing_clip_or_track_and_a_mesh_id_fail_naming_the_entry);
    RUN_TEST(test_a_scene_gives_each_entity_s_mesh_id_and_each_camera_s_lens);
    RUN_TEST(test_the_reader_refuses_an_entry_that_breaks_any_one_rule);
    RUN_TEST(test_scene_header_counts_are_independent_of_components);
    RUN_TEST(test_an_id_too_long_for_a_pack_name_is_cut_where_the_failure_names_it);
#ifndef DEVICE_BUILD
    RUN_TEST(test_authored_clip_ids_require_nonempty_terminated_text);
    RUN_TEST(test_authored_rows_and_refusals);
    RUN_TEST(test_encoder_authored_components_load_and_reach_runtime);
    RUN_TEST(test_authored_animator_clip_array_allocation_failure_releases_memory);
    RUN_TEST(test_encoder_authored_two_skin_allocation_failures_release_lighting);
    RUN_TEST(test_a_scene_the_tools_bake_loads_as_its_file_says);
#endif
#ifndef DEVICE_BUILD
    RUN_TEST(test_doc_scene_load_find_activate);
    RUN_TEST(test_doc_scene_missing_leaves_entities_and_camera_alone);
    RUN_TEST(test_doc_animator_blends_to_selected_clip);
    RUN_TEST(test_doc_present_modes_reach_camera);
    RUN_TEST(test_doc_small_fill_reaches_camera);
    RUN_TEST(test_scene_load_holds_its_pack_until_the_last_scene_from_it_unloads);
    RUN_TEST(test_a_scene_that_fails_to_load_does_not_hold_its_pack);
#endif
    RUN_TEST(test_a_load_with_no_memory_left_says_so_and_takes_nothing);
    RUN_TEST(test_a_full_manager_refuses_another_scene_until_one_is_unloaded);
    RUN_TEST(test_an_entity_is_found_by_its_name);
    RUN_TEST(test_skinned_mesh_requires_colours);
    RUN_TEST(test_lcol_shared_names);
    RUN_TEST(test_a_name_hashing_to_none_is_refused);
    RUN_TEST(test_finds_compare_name_hashes);
    RUN_TEST(test_lcol_instances_and_refusals);
    RUN_TEST(test_the_active_camera_draws_its_scenes_renderers);
    RUN_TEST(test_a_disabled_entity_is_not_drawn_and_enabling_it_draws_it_again);
    RUN_TEST(test_a_runtime_mesh_is_drawn_in_place_of_the_loaded_one);
    RUN_TEST(test_a_camera_takes_a_lens_shift_and_nothing_else_does);
    RUN_TEST(test_a_moved_entity_is_drawn_where_it_now_stands);
    RUN_TEST(test_a_camera_rolled_about_its_view_axis_draws_the_picture_turned);
    RUN_TEST(test_with_nothing_enabled_the_picture_is_left_alone);
    RUN_TEST(test_two_scenes_are_held_at_once_and_the_active_camera_decides_which_is_seen);
    RUN_TEST(test_unloading_the_active_scene_stops_drawing_and_leaves_the_other_alone);
    RUN_TEST(test_activating_a_camera_the_scene_does_not_have_changes_nothing);
    RUN_TEST(test_the_active_camera_flies_its_path_by_the_time_the_shell_gives_it);
    RUN_TEST(test_camera_animator_api_controls_clip_time);
    RUN_TEST(test_camera_aliases_advance_independently);
    RUN_TEST(test_a_frame_drawn_in_two_steps_is_the_frame_drawn_in_one);
    RUN_TEST(test_a_paused_scene_neither_advances_nor_draws_until_resumed);
    RUN_TEST(test_a_scale_renders_smaller_and_the_picture_is_upscaled_to_the_target);
    RUN_TEST(test_the_stats_count_what_the_last_draw_kept);
    RUN_TEST(test_leaving_the_app_unloads_every_scene_and_frees_the_scratch);
    RUN_TEST(test_a_turned_non_square_scene_places_the_coloured_instance);
    RUN_TEST(test_a_scaled_context_preserves_the_non_square_views_destination);
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
    RUN_TEST(test_the_scratch_is_the_finest_steps_raster_block);
    RUN_TEST(test_a_child_is_drawn_where_its_parent_places_it);
    RUN_TEST(test_a_child_moved_alone_stays_in_its_parents_frame);
    RUN_TEST(test_a_disabled_parent_hides_its_children_and_keeps_their_own_flags);
    RUN_TEST(test_an_entity_knows_its_parent_its_ancestors_and_its_world);
    RUN_TEST(test_a_camera_with_a_parent_is_refused);
    RUN_TEST(test_a_mirrored_transform_is_refused_by_name);
    RUN_TEST(test_a_world_is_the_parents_world_times_the_entitys_own);
    RUN_TEST(test_a_moved_grandparent_carries_its_grandchild);
    RUN_TEST(test_a_world_set_under_turned_parents_reads_back_exactly);
    RUN_TEST(test_an_uneven_scale_on_an_entity_with_children_is_refused);
    RUN_TEST(test_enabling_carries_down_every_level_and_stops_at_a_disabled_middle);
    RUN_TEST(test_a_light_or_skin_does_not_attach_under_a_parent);
    RUN_TEST(test_a_scene_with_parents_releases_everything_when_a_load_fails);
    scene_unload_all();
    (void)scene_set_take(lender);
}

SUITE_REGISTER(run_scene_suite);
