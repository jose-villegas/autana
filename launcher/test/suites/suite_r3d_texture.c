#ifndef DEVICE_BUILD
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_pack.h"
#include "image/pixel/bayer.h"
#include "image/pixel/rgb565.h"
#include "math/scalar/mathi.h"
#include "math/scalar/rng.h"
#include "r3d_pixel_lens.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "test_alloc.h"
#include "test_pack.h"
#include "unity.h"

#define SIDE         48
#define PIXELS       (SIDE * SIDE)
#define TARGETS      3 /* each a colour and a depth buffer */
#define TEX_LOG2     3
#define TEX_SIDE     (1 << TEX_LOG2)
#define TEX_LEVELS   3
#define PALETTE_OFF  R3D_TEXTURE_HEADER_SIZE
#define TEXELS_OFF   (PALETTE_OFF + (R3D_TEXTURE_COLOURS * R3D_TEXTURE_PALETTE_STRIDE))
#define TEX_BYTES    (TEXELS_OFF + 32 + 8 + 2)
#define RANDOM_SEED  4111U
#define RANDOM_TRIS  300
#define WHITE_INDEX  15
#define MEAN_INDEX   8 /* i * 16 == R3D_TEXTURE_ONE */
#define TINT_GREY    128
#define NEAR_DEPTH   0.5F
#define DEPTH_SPREAD 64

static uint16_t* buffers; /* colour then depth, twice */
static uint8_t* ltex;
static r3d_texture_t* texture;

static void
cleanup(void) {
    free(buffers);
    free(ltex);
    free(texture);
    buffers = NULL;
    ltex = NULL;
    texture = NULL;
}

static void
put_u16(uint8_t* at, uint16_t v) {
    memcpy(at, &v, sizeof v);
}

static void
put_u32(uint8_t* at, uint32_t v) {
    memcpy(at, &v, sizeof v);
}

/* An 8x8 INDEX4 texture with two mips: texel (x, y) of level 0 holds
 * index (x + y) & 15, each mip the index of its level; colour i is i * 16
 * in every channel, 15 being 1.0 white. */
static void
write_ltex(uint8_t* b) {
    memset(b, 0, TEX_BYTES);
    put_u16(b + R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_VERSION);
    put_u16(b + R3D_TEXTURE_AT_FORMAT, R3D_TEXTURE_INDEX4);
    b[R3D_TEXTURE_AT_SIZE_LOG2] = TEX_LOG2;
    b[R3D_TEXTURE_AT_LEVELS] = TEX_LEVELS;
    b[R3D_TEXTURE_AT_TINT] = TINT_GREY;
    b[R3D_TEXTURE_AT_TINT + 1] = TINT_GREY;
    b[R3D_TEXTURE_AT_TINT + 2] = TINT_GREY;
    put_u32(b + R3D_TEXTURE_AT_PALETTE_OFF, PALETTE_OFF);
    put_u32(b + R3D_TEXTURE_AT_TEXELS_OFF, TEXELS_OFF);
    for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
        const uint8_t level = i == WHITE_INDEX ? R3D_TEXTURE_ONE : (uint8_t)(i * 16);
        memset(b + PALETTE_OFF + (i * R3D_TEXTURE_PALETTE_STRIDE), level, 3);
    }
    uint8_t* at = b + TEXELS_OFF;
    for (int l = 0; l < TEX_LEVELS; l++) {
        const int side = TEX_SIDE >> l;
        for (int y = 0; y < side; y++) {
            for (int x = 0; x < side; x++) {
                const int o = (y * side) + x;
                const int index = l == 0 ? (x + y) & 15 : l;
                at[o >> 1] |= (uint8_t)(index << ((o & 1) * 4));
            }
        }
        at += (side * side) / 2;
    }
}

static void
fixture(void) {
    buffers = calloc((size_t)PIXELS * 2 * TARGETS, sizeof *buffers);
    ltex = malloc(TEX_BYTES);
    texture = malloc(sizeof *texture);
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(buffers);
    TEST_ASSERT_NOT_NULL(ltex);
    TEST_ASSERT_NOT_NULL(texture);
    write_ltex(ltex);
    const asset_view_t view = {ltex, TEX_BYTES};
    TEST_ASSERT_EQUAL(ASSET_OK, r3d_texture_from_asset(&view, texture));
}

static r3d_span_target_t
target_at(int which) {
    uint16_t* color = buffers + (which * 2 * PIXELS);
    return r3d_span_target(color, color + PIXELS, SIDE, 0, SIDE);
}

static void
clear(void) {
    memset(buffers, 0, sizeof(*buffers) * PIXELS * 2 * TARGETS);
}

static r3d_span_vertex_t
vertex(float x, float y, float z, float grey) {
    return (r3d_span_vertex_t){r3d_span_snap(x), r3d_span_snap(y), z, grey, grey, grey};
}

/* The triangle over the target's top-left half, every corner `grey`, drawn
 * into target `which` with the corners' `uv`. */
static void
draw_corner(int which, float grey, const vec2f_t uv[3]) {
    const r3d_span_target_t target = target_at(which);
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, grey), b = vertex(SIDE, 0, 0.5F, grey),
                            c = vertex(0, SIDE, 0.5F, grey);
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
}

static void
random_triangle(rng_t* random, r3d_span_vertex_t v[3], vec2f_t uv[3]) {
    for (int k = 0; k < 3; k++) {
        v[k] = (r3d_span_vertex_t){(int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   (int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   NEAR_DEPTH + ((float)rng_below(random, DEPTH_SPREAD) / 4096.0F),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256)};
        uv[k] = (vec2f_t){(float)rng_below(random, 64) - 32.0F, (float)rng_below(random, 64) - 32.0F};
    }
}

/* Every pixel a plain fill draws a textured fill draws, and no other: the
 * same edges decide coverage. Depth stays in a narrow band, as a sliver's
 * steeper depth step is clamped by the plain fill (its STEP_MAX) and can
 * leave it writing depth 0. */
static void
test_textured_covers_what_plain_covers(void) {
    fixture();
    rng_t random;
    rng_seed(&random, RANDOM_SEED);
    for (int t = 0; t < RANDOM_TRIS; t++) {
        r3d_span_vertex_t v[3];
        vec2f_t uv[3];
        random_triangle(&random, v, uv);
        clear();
        const r3d_span_target_t plain = target_at(0);
        const r3d_span_target_t textured = target_at(1);
        r3d_span_triangle(&plain, &v[0], &v[1], &v[2]);
        r3d_span_textured_triangle(&textured, &v[0], &v[1], &v[2], uv, texture);
        for (int i = 0; i < PIXELS; i++) {
            TEST_ASSERT_EQUAL_MESSAGE(buffers[PIXELS + i] != R3D_DEPTH_EMPTY,
                                      buffers[(3 * PIXELS) + i] != R3D_DEPTH_EMPTY, "coverage differs");
        }
    }
}

/* Two triangles sharing an edge, each filled alone, cover their square with
 * no pixel in both and none missed: the square a plain fill of both covers. */
static void
test_shared_edge_neither_cracks_nor_overlaps(void) {
    fixture();
    const vec2f_t uv[3] = {{0, 0}, {8, 0}, {8, 8}};
    const r3d_span_vertex_t a = vertex(3.3F, 2.7F, 0.5F, 100), b = vertex(41.1F, 5.2F, 0.5F, 100),
                            c = vertex(38.6F, 44.9F, 0.5F, 100), d = vertex(1.4F, 40.3F, 0.5F, 100);
    clear();
    const r3d_span_target_t first = target_at(0);
    const r3d_span_target_t second = target_at(1);
    r3d_span_textured_triangle(&first, &a, &b, &c, uv, texture);
    r3d_span_textured_triangle(&second, &a, &c, &d, uv, texture);
    bool* covered = calloc(PIXELS, sizeof *covered);
    TEST_ASSERT_NOT_NULL(covered);
    int overlap = 0;
    int textured = 0;
    for (int i = 0; i < PIXELS; i++) {
        const bool one = buffers[PIXELS + i] != R3D_DEPTH_EMPTY;
        const bool two = buffers[(3 * PIXELS) + i] != R3D_DEPTH_EMPTY;
        overlap += one && two;
        covered[i] = one || two;
        textured += covered[i];
    }
    clear();
    const r3d_span_target_t plain = target_at(0);
    r3d_span_triangle(&plain, &a, &b, &c);
    r3d_span_triangle(&plain, &a, &c, &d);
    int mismatched = 0;
    for (int i = 0; i < PIXELS; i++) {
        mismatched += covered[i] != (buffers[PIXELS + i] != R3D_DEPTH_EMPTY);
    }
    free(covered);
    TEST_ASSERT_GREATER_THAN(0, textured);
    TEST_ASSERT_EQUAL(0, overlap);
    TEST_ASSERT_EQUAL(0, mismatched);
}

/* One texel a pixel along x, constant v, under the brightest light: each
 * pixel shows the texel under its centre, its colour index times the mean
 * grey's albedo, within the colormap's last level. */
static void
test_affine_samples_the_texel_under_each_centre(void) {
    fixture();
    const float grey = 255.0F;
    const vec2f_t uv[3] = {{0, 0.5F}, {SIDE, 0.5F}, {0, 0.5F}};
    clear();
    draw_corner(0, grey, uv);
    for (int x = 0; x < SIDE / 2; x++) {
        const int index = x & (TEX_SIDE - 1);
        const int expected = index == WHITE_INDEX ? 255 : mathi_min((index * 16 * 255) / R3D_TEXTURE_ONE, 255);
        const uint16_t native = rgb565_swap(buffers[x]);
        TEST_ASSERT_INT_WITHIN(1, expected >> 3, (int)rgb565_r5(native));
        TEST_ASSERT_INT_WITHIN(1, expected >> 2, (int)rgb565_g6(native));
    }
}

/* Four texels a pixel picks level 2, whose texels all hold index 2. */
static void
test_minified_triangle_takes_the_matching_level(void) {
    fixture();
    const vec2f_t uv[3] = {{0, 0}, {4 * SIDE, 0}, {0, 4 * SIDE}};
    clear();
    draw_corner(0, UINT8_MAX, uv);
    const int expected = (2 * 16 * 255) / R3D_TEXTURE_ONE;
    TEST_ASSERT_INT_WITHIN(1, expected >> 2, (int)rgb565_g6(rgb565_swap(buffers[(4 * SIDE) + 4])));
}

/* Moved texels draw what the entry's did, and the entry is no longer read:
 * wiped after the move, the triangle still draws the same pixels. */
static void
test_moved_texels_draw_without_the_entry(void) {
    fixture();
    const vec2f_t uv[3] = {{0, 0}, {SIDE, 0}, {0, SIDE}};
    clear();
    draw_corner(0, UINT8_MAX, uv);
    const size_t bytes = r3d_texture_texel_bytes(texture);
    uint8_t* moved = malloc(bytes);
    TEST_ASSERT_NOT_NULL(moved);
    TEST_ASSERT_EQUAL_PTR(moved + bytes, r3d_texture_move_texels(texture, moved));
    memset(ltex, 0, TEX_BYTES);
    draw_corner(1, UINT8_MAX, uv);
    free(moved);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(buffers, buffers + (2 * PIXELS), 2 * PIXELS);
}

/* A texel at the material's mean (MEAN_INDEX) draws the vertex
 * colour itself: averaged over a 4x4 dither tile, within one level. */
static int
tile_green(void) {
    int sum = 0;
    for (int y = 4; y < 8; y++) {
        for (int x = 4; x < 8; x++) {
            sum += (int)rgb565_g6(rgb565_swap(buffers[(y * SIDE) + x]));
        }
    }
    return sum;
}

static void
test_texel_at_the_mean_draws_the_vertex_colour(void) {
    fixture();
    const float grey = 100.0F;
    /* Texel (7, 1) holds MEAN_INDEX, whose colour is R3D_TEXTURE_ONE. */
    const vec2f_t uv[3] = {{7.5F, 1.5F}, {7.5F, 1.5F}, {7.5F, 1.5F}};
    clear();
    _Static_assert(MEAN_INDEX * 16 == R3D_TEXTURE_ONE, "MEAN_INDEX's colour is the mean");
    draw_corner(0, grey, uv);
    const int tile = BAYER4_SIDE * BAYER4_SIDE;
    const int expected = ((int)grey * (int)RGB565_G6_MAX / UINT8_MAX) * tile;
    const int level_g6 = ((int)RGB565_G6_MAX / (R3D_TEXTURE_LIGHT_LEVELS - 1)) + 1;
    TEST_ASSERT_INT_WITHIN(level_g6 * tile, expected, tile_green());
}

static void
test_entry_rejects_bad_headers(void) {
    fixture();
    r3d_texture_t* t = texture;
    const asset_view_t view = {ltex, TEX_BYTES};
    ltex[R3D_TEXTURE_AT_FORMAT] = 2;
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, r3d_texture_from_asset(&view, t));
    write_ltex(ltex);
    ltex[R3D_TEXTURE_AT_LEVELS] = TEX_LOG2 + 1;
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, r3d_texture_from_asset(&view, t));
    write_ltex(ltex);
    const asset_view_t short_view = {ltex, TEX_BYTES - 1};
    TEST_ASSERT_EQUAL(ASSET_ERR_BOUNDS, r3d_texture_from_asset(&short_view, t));
    TEST_ASSERT_NULL(t->texels[0]);
    ltex[R3D_TEXTURE_AT_VERSION] = 2;
    TEST_ASSERT_EQUAL(ASSET_ERR_VERSION, r3d_texture_from_asset(&view, t));
}

/* A mesh's texturing: the LUVS entry, the pack holding its LTEX sibling, and
 * a scene of flat triangles seen at their own pixels, drawn through the
 * pipeline. */
#define MESH_ID           "m"
#define LUVS_BYTES_MAX    160
#define PACK_BYTES        512
#define UV_PAIR_BYTES     (2 * sizeof(int16_t))
#define READ_VERTICES     4
#define READ_UVS          6 /* the vertices', then the second cluster's two on chart 1 */
#define READ_CLUSTERS     2
#define READ_TRIANGLES    10 /* three bytes of triangle codes, the last one part full */
#define READ_LAST         9  /* the second cluster's last triangle, the one on a chart */
#define READ_CHART_OFFSET 2  /* the second cluster's first vertex, 2, reads chart 1 at 4 */
#define UV_U_UNITS        3  /* 1/16 texel */
#define UV_V_UNITS        5
#define SCENE_VERTICES    3
#define SCENE_TRIANGLES   2
#define SCENE_LIGHT       200
#define SCENE_Z           2.0F
#define BEHIND_Z          (-2.0F)
#define UV_SPAN_UNITS     (TEX_SIDE << R3D_TEXTURE_UV_SHIFT) /* one texture side along a triangle's legs */
#define LEFT_COLUMNS      (SIDE / 2)
#define DISTINCT_MORE     3
#define COVERED_MIN       100
#define FACE_COLOUR       0x07E0 /* RGB565 green, not a colour the texture makes */

typedef struct {
    int16_t positions[SCENE_VERTICES * SCENE_TRIANGLES][3];
    uint8_t colors[SCENE_VERTICES * SCENE_TRIANGLES][3];
    uint16_t triangles[SCENE_TRIANGLES][3];
    int16_t uvs[R3D_TEXTURE_CHARTS * SCENE_VERTICES * SCENE_TRIANGLES][2]; /* chart 0's, then chart 1's */
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
    r3d_pipeline_vertex_t cs[SCENE_VERTICES * SCENE_TRIANGLES];
    r3d_pipeline_rows_t rows[1];
    r3d_lit_mesh_t mesh;
    r3d_texture_t material;
    r3d_mesh_texturing_t texturing;
    uint8_t luvs[LUVS_BYTES_MAX];
    uint8_t* pack_base;
    void* pack_raw;
    asset_pack_t pack;
    r3d_pipeline_work_t* work;
} scene_t;

static scene_t* scene;

static void
scene_cleanup(void) {
    if (scene != NULL) {
        free(scene->work);
        test_free_aligned(scene->pack_raw);
    }
    free(scene);
    scene = NULL;
    cleanup();
}

/* An LUVS entry's sections, each as the reader exposes it. */
typedef struct {
    uint32_t vertices, uv_count, clusters, triangles;
    const int16_t (*uvs)[2];
    const int32_t* shifts;
    const uint8_t (*materials)[R3D_TEXTURE_CHARTS];
    const uint8_t* codes;
} luvs_parts_t;

/* The LUVS entry of one material: the layout docs/assets/formats/LUVS.md
 * gives, the sections packed one after another; returns its size. */
static uint32_t
write_luvs(uint8_t* b, const luvs_parts_t* in) {
    const uint32_t uvs_off = R3D_TEXTURE_UVS_HEADER_SIZE;
    const uint32_t shifts_off = uvs_off + (in->uv_count * UV_PAIR_BYTES);
    const uint32_t clusters_off = shifts_off + (in->clusters * sizeof(int32_t));
    const uint32_t triangles_off = clusters_off + (in->clusters * R3D_TEXTURE_CHARTS);
    const uint32_t code_bytes =
        (in->triangles + R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE - 1U) / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE;
    memset(b, 0, LUVS_BYTES_MAX);
    put_u16(b + R3D_TEXTURE_UVS_AT_VERSION, R3D_TEXTURE_UVS_VERSION);
    put_u16(b + R3D_TEXTURE_UVS_AT_MATERIAL_COUNT, 1);
    put_u32(b + R3D_TEXTURE_UVS_AT_VERTEX_COUNT, in->vertices);
    put_u32(b + R3D_TEXTURE_UVS_AT_CLUSTER_COUNT, in->clusters);
    put_u32(b + R3D_TEXTURE_UVS_AT_TRIANGLE_COUNT, in->triangles);
    put_u32(b + R3D_TEXTURE_UVS_AT_UV_COUNT, in->uv_count);
    put_u32(b + R3D_TEXTURE_UVS_AT_UVS_OFF, uvs_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_SHIFTS_OFF, shifts_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, clusters_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, triangles_off);
    memcpy(b + uvs_off, in->uvs, in->uv_count * UV_PAIR_BYTES);
    memcpy(b + shifts_off, in->shifts, in->clusters * sizeof(int32_t));
    memcpy(b + clusters_off, in->materials, in->clusters * R3D_TEXTURE_CHARTS);
    memcpy(b + triangles_off, in->codes, code_bytes);
    return triangles_off + code_bytes;
}

/* Triangle `t`'s `code` placed in a code byte. */
static uint8_t
code_at(int t, unsigned code) {
    return (uint8_t)(code << (((unsigned)t % R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE) * R3D_TEXTURE_UVS_CODE_BITS));
}

/* The scene and a pack of the one LTEX the mesh "m" names. */
static scene_t*
scene_fixture(void) {
    fixture();
    scene = calloc(1, sizeof *scene);
    suite_set_test_cleanup(scene_cleanup);
    TEST_ASSERT_NOT_NULL(scene);
    scene->work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(scene->work);
    scene->pack_base = test_alloc_aligned(PACK_BYTES, ASSET_PACK_BASE_ALIGN, &scene->pack_raw);
    TEST_ASSERT_NOT_NULL(scene->pack_base);
    test_pack_t built = test_pack_begin(scene->pack_base, PACK_BYTES, 1);
    memcpy(test_pack_add(&built, MESH_ID R3D_TEXTURE_MATERIAL_SUFFIX "0", R3D_TEXTURE_ASSET, TEX_BYTES), ltex,
           TEX_BYTES);
    const uint32_t total = test_pack_finish(&built);
    TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(&scene->pack, scene->pack_base, total));
    return scene;
}

static asset_status_t
open_texturing(scene_t* s, uint32_t size, const r3d_lit_mesh_t* mesh) {
    const asset_view_t view = {s->luvs, size};
    return r3d_texture_texturing_from_asset(&view, &s->pack, MESH_ID, mesh, &s->material, 1, &s->texturing);
}

static void
corner(scene_t* s, int k, float x, float y, float z) {
    s->positions[k][0] = (int16_t)(x * R3D_SUBPIXEL * z);
    s->positions[k][1] = (int16_t)(y * R3D_SUBPIXEL * z);
    s->positions[k][2] = (int16_t)z;
    memset(s->colors[k], SCENE_LIGHT, sizeof s->colors[k]);
}

/* The mesh's UVs: on chart 0 the texture's first side along each
 * triangle's legs, on chart 1 `chart_1_sides` of them. */
static void
leg_uvs(scene_t* s, int triangles, int chart_1_sides) {
    for (int t = 0; t < triangles; t++) {
        for (int chart = 0; chart < R3D_TEXTURE_CHARTS; chart++) {
            const int16_t span = (int16_t)(chart == 0 ? UV_SPAN_UNITS : UV_SPAN_UNITS * chart_1_sides);
            const int16_t uv[SCENE_VERTICES][2] = {{0, 0}, {span, 0}, {0, span}};
            memcpy(s->uvs[(chart * triangles * SCENE_VERTICES) + (t * SCENE_VERTICES)], uv, sizeof uv);
        }
        for (int k = 0; k < SCENE_VERTICES; k++) {
            s->triangles[t][k] = (uint16_t)((t * SCENE_VERTICES) + k);
        }
    }
}

/* Builds the one-cluster mesh of `triangles` triangles whose corners are set,
 * with its texturing read from an LUVS entry of triangle codes `codes`; the
 * cluster has chart 1 when `charts` is 2. */
static void
scene_mesh(scene_t* s, int triangles, uint8_t codes, int charts) {
    const int vertices = triangles * SCENE_VERTICES;
    const box3s_t reach = {{-INT16_MAX, -INT16_MAX, -8}, {INT16_MAX, INT16_MAX, 8}};
    s->cluster = (r3d_lit_cluster_t){0, (uint16_t)vertices, 0, (uint16_t)triangles, reach, true};
    s->node = (r3d_lit_node_t){reach, 0, 1, true};
    s->mesh = (r3d_lit_mesh_t){.positions = s->positions,
                               .colors = s->colors,
                               .triangles = s->triangles,
                               .clusters = &s->cluster,
                               .nodes = &s->node,
                               .vertex_count = vertices,
                               .triangle_count = triangles,
                               .cluster_count = 1,
                               .node_count = 1,
                               .position_scale = 1};
    const int32_t shifts[1] = {vertices};
    const uint8_t materials[1][R3D_TEXTURE_CHARTS] = {{0, charts > 1 ? 0 : R3D_TEXTURE_NONE}};
    const luvs_parts_t parts = {(uint32_t)vertices,
                                (uint32_t)(R3D_TEXTURE_CHARTS * vertices),
                                1,
                                (uint32_t)triangles,
                                s->uvs,
                                shifts,
                                materials,
                                &codes};
    const uint32_t size = write_luvs(s->luvs, &parts);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &s->mesh));
    s->mesh.texturing = &s->texturing;
}

/* Draws the scene into target `which`, with its texturing or plain. */
static void
draw_scene(scene_t* s, int which, bool textured) {
    const r3d_lens_t lens = r3d_pixel_lens(SIDE, SIDE);
    const uint16_t visible[1] = {0};
    const r3d_span_target_t target = target_at(which);
    r3d_pipeline_transform(&s->mesh, &lens, visible, 1, s->cs, s->rows, false);
    s->mesh.texturing = textured ? &s->texturing : NULL;
    r3d_pipeline_draw(&s->mesh, &lens, visible, 1, s->cs, s->rows, &target, s->work);
}

static const uint16_t*
colours_of(int which) {
    return buffers + (which * 2 * PIXELS);
}

static const uint16_t*
depths_of(int which) {
    return colours_of(which) + PIXELS;
}

static int
covered_pixels(int which) {
    int n = 0;
    for (int i = 0; i < PIXELS; i++) {
        n += depths_of(which)[i] != R3D_DEPTH_EMPTY;
    }
    return n;
}

static int
distinct_colours(int which) {
    bool* seen = calloc((size_t)UINT16_MAX + 1, sizeof *seen);
    TEST_ASSERT_NOT_NULL(seen);
    int n = 0;
    for (int i = 0; i < PIXELS; i++) {
        const uint16_t c = colours_of(which)[i];
        if (depths_of(which)[i] != R3D_DEPTH_EMPTY && !seen[c]) {
            seen[c] = true;
            n++;
        }
    }
    free(seen);
    return n;
}

/* An entry for READ_VERTICES vertices in READ_CLUSTERS clusters and
 * READ_TRIANGLES triangles: the first cluster, vertices 0-1 and triangles
 * 0-4, on chart 0 alone; the second, vertices 2-3 and triangles 5-9, on two
 * charts; every triangle plain but READ_LAST, on the second cluster's chart
 * 1. */
static const r3d_lit_cluster_t read_clusters[READ_CLUSTERS] = {
    {.vertex_first = 0, .vertex_count = 2, .triangle_first = 0, .triangle_count = 5},
    {.vertex_first = 2, .vertex_count = 2, .triangle_first = 5, .triangle_count = 5}};

static uint32_t
write_read_entry(scene_t* s) {
    const int16_t uvs[READ_UVS][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {UV_U_UNITS, UV_V_UNITS}};
    const int32_t shifts[READ_CLUSTERS] = {0, READ_CHART_OFFSET};
    const uint8_t materials[READ_CLUSTERS][R3D_TEXTURE_CHARTS] = {{0, R3D_TEXTURE_NONE}, {0, 0}};
    uint8_t codes[3] = {0};
    codes[READ_LAST / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE] = code_at(READ_LAST, 2);
    const luvs_parts_t parts = {READ_VERTICES, READ_UVS, READ_CLUSTERS, READ_TRIANGLES, uvs, shifts, materials, codes};
    return write_luvs(s->luvs, &parts);
}

static r3d_lit_mesh_t
read_mesh(void) {
    return (r3d_lit_mesh_t){.clusters = read_clusters,
                            .vertex_count = READ_VERTICES,
                            .cluster_count = READ_CLUSTERS,
                            .triangle_count = READ_TRIANGLES};
}

static void
test_luvs_entry_opens_and_exposes_its_codes_and_uvs(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    TEST_ASSERT_EQUAL(1, s->texturing.material_count);
    TEST_ASSERT_EQUAL_PTR(&s->material, s->texturing.materials);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[0][0]);
    TEST_ASSERT_EQUAL_UINT8(R3D_TEXTURE_NONE, s->texturing.cluster_material[0][1]);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[1][1]);
    for (int t = 0; t < READ_TRIANGLES; t++) {
        TEST_ASSERT_EQUAL_MESSAGE(t == READ_LAST ? 2U : R3D_TEXTURE_PLAIN, r3d_texture_triangle_code(&s->texturing, t),
                                  "a triangle code");
    }
    /* Chart 1 reads the vertex's UV past its shift, in 1/16 texel; chart 0
     * reads the vertex's own. */
    const vec2f_t on_1 = r3d_texture_uv(&s->texturing, 1, 1, READ_VERTICES - 1);
    TEST_ASSERT_EQUAL_FLOAT((float)UV_U_UNITS / (1 << R3D_TEXTURE_UV_SHIFT), on_1.x);
    TEST_ASSERT_EQUAL_FLOAT((float)UV_V_UNITS / (1 << R3D_TEXTURE_UV_SHIFT), on_1.y);
    const vec2f_t on_0 = r3d_texture_uv(&s->texturing, 1, 0, READ_VERTICES - 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, on_0.x);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, on_0.y);
}

static void
expect_refused(scene_t* s, uint32_t size, const r3d_lit_mesh_t* mesh, asset_status_t expected, const char* why) {
    memset(&s->texturing, 0xFF, sizeof s->texturing);
    TEST_ASSERT_EQUAL_MESSAGE(expected, open_texturing(s, size, mesh), why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.uvs, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.cluster_material, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.cluster_shift, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.triangle_code, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.materials, why);
    TEST_ASSERT_EQUAL_MESSAGE(0, s->texturing.material_count, why);
}

static void
test_luvs_entry_is_refused_when_its_counts_do_not_match_the_mesh(void) {
    scene_t* s = scene_fixture();
    const uint32_t size = write_read_entry(s);
    r3d_lit_mesh_t mesh = read_mesh();
    mesh.triangle_count++;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "triangle count");
    mesh = read_mesh();
    mesh.vertex_count++;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "vertex count");
    mesh = read_mesh();
    mesh.cluster_count++;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "cluster count");
}

static void
test_luvs_entry_must_end_exactly_after_its_triangle_codes(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    expect_refused(s, size + 1, &mesh, ASSET_ERR_BOUNDS, "one byte more");
    expect_refused(s, size - 1, &mesh, ASSET_ERR_BOUNDS, "one byte less");
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

static void
test_luvs_cluster_material_past_the_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    const uint32_t clusters_off = asset_read_u32(s->luvs + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF);
    s->luvs[clusters_off + 1] = 1; /* one material: index 1 is past it, NONE is not */
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "material index");
}

/* Where each section of the read entry starts, and the bytes a test edits. */
static uint32_t
header_word(const scene_t* s, uint32_t at) {
    return asset_read_u32(s->luvs + at);
}

static void
test_luvs_code_naming_chart_1_of_a_one_chart_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_TRIANGLES_OFF)] = code_at(0, 2);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "a code naming chart 1 of a cluster with one chart");
}

static void
test_luvs_chart_1_after_a_chart_with_no_material_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF) + R3D_TEXTURE_CHARTS] = R3D_TEXTURE_NONE;
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1 after a chart with no material");
}

static void
test_luvs_chart_1_uvs_past_the_entrys_are_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF) + sizeof(int32_t), READ_CHART_OFFSET + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1's UVs one past the entry's");
}

/* The shift that puts the second cluster's chart 1 `moved` UVs from where the
 * entry has them. */
static void
shift_cluster_1(scene_t* s, int32_t moved) {
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF) + sizeof(int32_t),
            (uint32_t)(int32_t)(READ_CHART_OFFSET + moved));
}

static void
test_luvs_negative_shift_of_a_two_chart_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    shift_cluster_1(s, -(int32_t)(read_clusters[1].vertex_first + READ_CHART_OFFSET + 1));
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1's UVs one before the entry's");
}

static void
test_luvs_chart_1_uvs_fitting_the_entry_exactly_at_each_end_open(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    TEST_ASSERT_EQUAL_UINT32(READ_UVS,
                             read_clusters[1].vertex_first + READ_CHART_OFFSET + read_clusters[1].vertex_count);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    shift_cluster_1(s, -(int32_t)(read_clusters[1].vertex_first + READ_CHART_OFFSET));
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

static void
test_luvs_one_chart_cluster_opens_with_a_shift_out_of_range(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    const int32_t far = 1000;
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF), (uint32_t)far);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    put_u32(s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF), (uint32_t)-far);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

/* Triangle `t`'s code byte, set to `code` with its neighbours' bits cleared. */
static void
set_code(scene_t* s, int t, unsigned code) {
    uint8_t* byte =
        s->luvs + header_word(s, R3D_TEXTURE_UVS_AT_TRIANGLES_OFF) + (t / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE);
    *byte = code_at(t, code);
}

/* The read entry with no triangle on a chart, to set one code in. */
static uint32_t
write_plain_entry(scene_t* s) {
    const uint32_t size = write_read_entry(s);
    for (int t = 0; t < READ_TRIANGLES; t += R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE) {
        set_code(s, t, R3D_TEXTURE_PLAIN);
    }
    return size;
}

static void
test_luvs_code_3_in_a_two_chart_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    set_code(s, 7, R3D_TEXTURE_CHARTS + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "code 3 in a cluster with two charts");
}

static void
test_luvs_bad_code_in_the_second_cluster_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    set_code(s, read_clusters[1].triangle_first, R3D_TEXTURE_CHARTS + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "a bad code in cluster 1");
}

static void
test_luvs_bad_code_on_the_last_partly_packed_triangle_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    TEST_ASSERT_NOT_EQUAL(0, READ_TRIANGLES % R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE);
    const uint32_t size = write_plain_entry(s);
    set_code(s, READ_TRIANGLES - 1, R3D_TEXTURE_CHARTS + 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "a bad code on the last triangle");
}

static void
test_luvs_codes_up_to_the_clusters_last_chart_open(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    set_code(s, 0, 1);
    set_code(s, read_clusters[1].triangle_first, R3D_TEXTURE_CHARTS);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
}

/* The read entry with chart `chart` of cluster `c` naming material `k`. */
static uint32_t
write_naming(scene_t* s, int c, int chart, uint8_t k) {
    const uint32_t size = write_read_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF) + (c * R3D_TEXTURE_CHARTS) + chart] = k;
    return size;
}

static void
test_luvs_chart_0_naming_the_material_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 0, 0, 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 0 of cluster 0 naming material 1 of 1");
}

static void
test_luvs_second_clusters_chart_0_naming_the_material_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 1, 0, 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 0 of cluster 1 naming material 1 of 1");
}

static void
test_luvs_second_clusters_chart_1_naming_the_material_count_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 1, 1, 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "chart 1 of cluster 1 naming material 1 of 1");
}

static void
test_luvs_material_one_below_the_count_opens_on_every_chart(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_naming(s, 1, 1, 0);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[0][0]);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[1][0]);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[1][1]);
}

static void
test_luvs_with_fewer_uvs_than_vertices_is_refused(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_plain_entry(s);
    s->luvs[header_word(s, R3D_TEXTURE_UVS_AT_CLUSTERS_OFF) + R3D_TEXTURE_CHARTS + 1] = R3D_TEXTURE_NONE;
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    put_u32(s->luvs + R3D_TEXTURE_UVS_AT_UV_COUNT, READ_VERTICES - 1);
    expect_refused(s, size, &mesh, ASSET_ERR_FORMAT, "one UV fewer than vertices");
}

/* The read entry with the section offset at `at` set to `value`. */
static void
expect_offset_refused(scene_t* s, uint32_t at, uint32_t value, const char* why) {
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    put_u32(s->luvs + at, value);
    expect_refused(s, size, &mesh, ASSET_ERR_BOUNDS, why);
}

/* Past the written entry, aligned for every section. */
#define PAST_ENTRY 76U

static void
test_luvs_misaligned_uvs_offset_is_refused(void) {
    scene_t* s = scene_fixture();
    (void)write_read_entry(s);
    expect_offset_refused(s, R3D_TEXTURE_UVS_AT_UVS_OFF, header_word(s, R3D_TEXTURE_UVS_AT_UVS_OFF) + 1,
                          "UVs at an odd byte");
}

static void
test_luvs_misaligned_shifts_offset_is_refused(void) {
    scene_t* s = scene_fixture();
    (void)write_read_entry(s);
    expect_offset_refused(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF, header_word(s, R3D_TEXTURE_UVS_AT_SHIFTS_OFF) + 2,
                          "shifts off a 4-byte boundary");
}

static void
test_luvs_uvs_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_UVS_OFF, 0, "UVs over the header");
}

static void
test_luvs_shifts_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_SHIFTS_OFF, 0, "shifts over the header");
}

static void
test_luvs_clusters_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, 0, "cluster materials over the header");
}

static void
test_luvs_triangles_offset_inside_the_header_is_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, 0, "triangle codes over the header");
}

static void
test_luvs_uvs_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_UVS_OFF, PAST_ENTRY, "UVs past the entry");
}

static void
test_luvs_shifts_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_SHIFTS_OFF, PAST_ENTRY, "shifts past the entry");
}

static void
test_luvs_cluster_materials_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, PAST_ENTRY,
                          "cluster materials past the entry");
}

static void
test_luvs_triangle_codes_running_past_the_entry_are_refused(void) {
    expect_offset_refused(scene_fixture(), R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, PAST_ENTRY,
                          "triangle codes past the entry");
}

static void
test_luvs_names_a_material_the_pack_lacks(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    const asset_view_t view = {s->luvs, size};
    memset(&s->texturing, 0xFF, sizeof s->texturing);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND, r3d_texture_texturing_from_asset(&view, &s->pack, "other", &mesh,
                                                                            &s->material, 1, &s->texturing));
    TEST_ASSERT_NULL(s->texturing.uvs);
    TEST_ASSERT_NULL(s->texturing.triangle_code);
}

/* One cluster of two disjoint triangles, the left one in the left half,
 * each drawn as its code in `codes` says; chart 1 maps `chart_1_sides`
 * texture sides along the legs. */
static void
two_triangles(scene_t* s, uint8_t codes, int chart_1_sides) {
    const float left[3][2] = {{2, 4}, {22, 4}, {12, 40}};
    const float right[3][2] = {{26, 4}, {46, 4}, {36, 40}};
    for (int k = 0; k < SCENE_VERTICES; k++) {
        corner(s, k, left[k][0], left[k][1], SCENE_Z);
        corner(s, SCENE_VERTICES + k, right[k][0], right[k][1], SCENE_Z);
    }
    leg_uvs(s, SCENE_TRIANGLES, chart_1_sides);
    scene_mesh(s, SCENE_TRIANGLES, codes, R3D_TEXTURE_CHARTS);
}

/* Pixels of target 1 that differ from target 0's plain draw, in the left
 * half and the right. */
static void
differing(int* left, int* right) {
    *left = 0;
    *right = 0;
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(depths_of(0)[i] != R3D_DEPTH_EMPTY, depths_of(1)[i] != R3D_DEPTH_EMPTY, "coverage");
        const bool differs = colours_of(0)[i] != colours_of(1)[i];
        (*((i % SIDE) < LEFT_COLUMNS ? left : right)) += differs;
    }
}

static void
expect_drawn_textured(bool marked, int differing_pixels, const char* which) {
    if (marked) {
        TEST_ASSERT_GREATER_THAN_MESSAGE(0, differing_pixels, which);
    } else {
        TEST_ASSERT_EQUAL_MESSAGE(0, differing_pixels, which);
    }
}

static void
test_only_triangles_on_a_chart_draw_textured(void) {
    const unsigned codes[][SCENE_TRIANGLES] = {
        {1, R3D_TEXTURE_PLAIN}, {R3D_TEXTURE_PLAIN, 2}, {R3D_TEXTURE_PLAIN, R3D_TEXTURE_PLAIN}};
    for (size_t m = 0; m < sizeof codes / sizeof codes[0]; m++) {
        scene_t* s = scene_fixture();
        two_triangles(s, code_at(0, codes[m][0]) | code_at(1, codes[m][1]), 1);
        clear();
        draw_scene(s, 0, false);
        draw_scene(s, 1, true);
        int left, right;
        differing(&left, &right);
        expect_drawn_textured(codes[m][0] != R3D_TEXTURE_PLAIN, left, "the left triangle");
        expect_drawn_textured(codes[m][1] != R3D_TEXTURE_PLAIN, right, "the right triangle");
        scene_cleanup();
    }
}

/* A triangle on chart 1 reads chart 1's UVs: drawn on chart 1 it matches
 * itself drawn on chart 0 with the same UVs, and not chart 0's own. */
static void
test_a_triangle_on_chart_1_draws_with_chart_1s_uvs(void) {
    const int sides = 2;
    scene_t* s = scene_fixture();
    two_triangles(s, code_at(0, 2), sides);
    clear();
    draw_scene(s, 0, true);
    scene_mesh(s, SCENE_TRIANGLES, code_at(0, 1), R3D_TEXTURE_CHARTS);
    draw_scene(s, 1, true);
    for (int k = 0; k < SCENE_TRIANGLES * SCENE_VERTICES; k++) {
        for (int i = 0; i < 2; i++) {
            s->uvs[k][i] = (int16_t)(s->uvs[k][i] * sides);
        }
    }
    scene_mesh(s, SCENE_TRIANGLES, code_at(0, 1), R3D_TEXTURE_CHARTS);
    draw_scene(s, 2, true);
    int differs_from_chart_0 = 0;
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(colours_of(2)[i], colours_of(0)[i], "chart 1 against the same UVs on chart 0");
        differs_from_chart_0 += colours_of(0)[i] != colours_of(1)[i];
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, differs_from_chart_0, "chart 1 drew chart 0's UVs");
}

static void
test_near_clipped_textured_triangle_keeps_its_coverage_and_its_pattern(void) {
    scene_t* s = scene_fixture();
    /* Two corners in front, one behind the near plane. */
    corner(s, 0, 8, 20, SCENE_Z);
    corner(s, 1, 40, 20, SCENE_Z);
    corner(s, 2, 0, 0, BEHIND_Z);
    leg_uvs(s, 1, 1);
    scene_mesh(s, 1, 1, 1);
    clear();
    draw_scene(s, 0, false);
    draw_scene(s, 1, true);
    int covered = 0;
    int differs = 0;
    for (int i = 0; i < PIXELS; i++) {
        const bool plain = depths_of(0)[i] != R3D_DEPTH_EMPTY;
        TEST_ASSERT_EQUAL_MESSAGE(plain, depths_of(1)[i] != R3D_DEPTH_EMPTY,
                                  "the clipped draws cover different pixels");
        covered += plain;
        differs += colours_of(0)[i] != colours_of(1)[i];
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, covered, "the clipped triangle covers too little to tell");
    TEST_ASSERT_GREATER_THAN_MESSAGE(covered / 2, differs, "the part in front is not textured");
    TEST_ASSERT_GREATER_THAN_MESSAGE(distinct_colours(0) + DISTINCT_MORE, distinct_colours(1),
                                     "the clipped triangle's UVs do not vary across it");
}

/* A mesh with a flat colour per triangle draws plain whatever its codes say:
 * the textured draw covers and colours the pixels the plain one does. */
static void
test_a_mesh_with_face_colours_draws_its_textured_triangles_plain(void) {
    const uint16_t faces[SCENE_TRIANGLES] = {FACE_COLOUR, FACE_COLOUR};
    scene_t* s = scene_fixture();
    two_triangles(s, code_at(0, 1) | code_at(1, 2), 1);
    clear();
    draw_scene(s, 0, false);
    draw_scene(s, 1, true);
    int left, right;
    differing(&left, &right);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, left + right, "the mesh without face colours does not draw textured");
    s->mesh.face_colors = faces;
    clear();
    draw_scene(s, 0, false);
    draw_scene(s, 1, true);
    differing(&left, &right);
    TEST_ASSERT_EQUAL_MESSAGE(0, left + right, "a triangle with a face colour drew textured");
    TEST_ASSERT_GREATER_THAN_MESSAGE(COVERED_MIN, covered_pixels(1), "nothing was drawn");
}

void
run_r3d_texture_suite(void) {
    RUN_TEST(test_textured_covers_what_plain_covers);
    RUN_TEST(test_shared_edge_neither_cracks_nor_overlaps);
    RUN_TEST(test_affine_samples_the_texel_under_each_centre);
    RUN_TEST(test_minified_triangle_takes_the_matching_level);
    RUN_TEST(test_moved_texels_draw_without_the_entry);
    RUN_TEST(test_texel_at_the_mean_draws_the_vertex_colour);
    RUN_TEST(test_entry_rejects_bad_headers);
    RUN_TEST(test_luvs_entry_opens_and_exposes_its_codes_and_uvs);
    RUN_TEST(test_luvs_entry_is_refused_when_its_counts_do_not_match_the_mesh);
    RUN_TEST(test_luvs_entry_must_end_exactly_after_its_triangle_codes);
    RUN_TEST(test_luvs_cluster_material_past_the_count_is_refused);
    RUN_TEST(test_luvs_code_naming_chart_1_of_a_one_chart_cluster_is_refused);
    RUN_TEST(test_luvs_chart_1_after_a_chart_with_no_material_is_refused);
    RUN_TEST(test_luvs_chart_1_uvs_past_the_entrys_are_refused);
    RUN_TEST(test_luvs_negative_shift_of_a_two_chart_cluster_is_refused);
    RUN_TEST(test_luvs_chart_1_uvs_fitting_the_entry_exactly_at_each_end_open);
    RUN_TEST(test_luvs_one_chart_cluster_opens_with_a_shift_out_of_range);
    RUN_TEST(test_luvs_code_3_in_a_two_chart_cluster_is_refused);
    RUN_TEST(test_luvs_bad_code_in_the_second_cluster_is_refused);
    RUN_TEST(test_luvs_bad_code_on_the_last_partly_packed_triangle_is_refused);
    RUN_TEST(test_luvs_codes_up_to_the_clusters_last_chart_open);
    RUN_TEST(test_luvs_chart_0_naming_the_material_count_is_refused);
    RUN_TEST(test_luvs_second_clusters_chart_0_naming_the_material_count_is_refused);
    RUN_TEST(test_luvs_second_clusters_chart_1_naming_the_material_count_is_refused);
    RUN_TEST(test_luvs_material_one_below_the_count_opens_on_every_chart);
    RUN_TEST(test_luvs_with_fewer_uvs_than_vertices_is_refused);
    RUN_TEST(test_luvs_misaligned_uvs_offset_is_refused);
    RUN_TEST(test_luvs_misaligned_shifts_offset_is_refused);
    RUN_TEST(test_luvs_uvs_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_shifts_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_clusters_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_triangles_offset_inside_the_header_is_refused);
    RUN_TEST(test_luvs_uvs_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_shifts_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_cluster_materials_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_triangle_codes_running_past_the_entry_are_refused);
    RUN_TEST(test_luvs_names_a_material_the_pack_lacks);
    RUN_TEST(test_only_triangles_on_a_chart_draw_textured);
    RUN_TEST(test_a_triangle_on_chart_1_draws_with_chart_1s_uvs);
    RUN_TEST(test_near_clipped_textured_triangle_keeps_its_coverage_and_its_pattern);
    RUN_TEST(test_a_mesh_with_face_colours_draws_its_textured_triangles_plain);
}

SUITE_REGISTER(run_r3d_texture_suite);
#endif
