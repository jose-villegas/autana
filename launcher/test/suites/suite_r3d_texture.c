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
    buffers = calloc(PIXELS * 4, sizeof *buffers);
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
    memset(buffers, 0, sizeof(*buffers) * PIXELS * 4);
}

static r3d_span_vertex_t
vertex(float x, float y, float z, float grey) {
    return (r3d_span_vertex_t){r3d_span_snap(x), r3d_span_snap(y), z, grey, grey, grey};
}

static void
random_triangle(rng_t* random, r3d_span_vertex_t v[3], r3d_span_uv_t uv[3]) {
    for (int k = 0; k < 3; k++) {
        v[k] = (r3d_span_vertex_t){(int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   (int32_t)rng_below(random, (SIDE + 16) * R3D_SUBPIXEL) - (8 * R3D_SUBPIXEL),
                                   NEAR_DEPTH + ((float)rng_below(random, DEPTH_SPREAD) / 4096.0F),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256),
                                   (float)rng_below(random, 256)};
        uv[k] = (r3d_span_uv_t){(float)rng_below(random, 64) - 32.0F, (float)rng_below(random, 64) - 32.0F};
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
        r3d_span_uv_t uv[3];
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
    const r3d_span_uv_t uv[3] = {{0, 0}, {8, 0}, {8, 8}};
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
    const r3d_span_target_t target = target_at(0);
    const float grey = 255.0F;
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, grey), b = vertex(SIDE, 0, 0.5F, grey),
                            c = vertex(0, SIDE, 0.5F, grey);
    const r3d_span_uv_t uv[3] = {{0, 0.5F}, {SIDE, 0.5F}, {0, 0.5F}};
    clear();
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
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
    const r3d_span_target_t target = target_at(0);
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, 255), b = vertex(SIDE, 0, 0.5F, 255), c = vertex(0, SIDE, 0.5F, 255);
    const r3d_span_uv_t uv[3] = {{0, 0}, {4 * SIDE, 0}, {0, 4 * SIDE}};
    clear();
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
    const int expected = (2 * 16 * 255) / R3D_TEXTURE_ONE;
    TEST_ASSERT_INT_WITHIN(1, expected >> 2, (int)rgb565_g6(rgb565_swap(buffers[(4 * SIDE) + 4])));
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
    const r3d_span_target_t target = target_at(0);
    const float grey = 100.0F;
    const r3d_span_vertex_t a = vertex(0, 0, 0.5F, grey), b = vertex(SIDE, 0, 0.5F, grey),
                            c = vertex(0, SIDE, 0.5F, grey);
    /* Texel (7, 1) holds MEAN_INDEX, whose colour is R3D_TEXTURE_ONE. */
    const r3d_span_uv_t uv[3] = {{7.5F, 1.5F}, {7.5F, 1.5F}, {7.5F, 1.5F}};
    clear();
    _Static_assert(MEAN_INDEX * 16 == R3D_TEXTURE_ONE, "MEAN_INDEX's colour is the mean");
    r3d_span_textured_triangle(&target, &a, &b, &c, uv, texture);
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
#define MESH_ID         "m"
#define LUVS_BYTES_MAX  160
#define PACK_BYTES      512
#define UV_PAIR_BYTES   (2 * sizeof(int16_t))
#define READ_VERTICES   4
#define READ_CLUSTERS   2
#define READ_TRIANGLES  10 /* two bytes of triangle bits */
#define READ_LAST_BIT   9  /* bit 1 of byte 1 */
#define ORIGIN_U        1
#define ORIGIN_V        2
#define UV_U_UNITS      3 /* 1/16 texel */
#define UV_V_UNITS      5
#define SCENE_VERTICES  3
#define SCENE_TRIANGLES 2
#define SCENE_LIGHT     200
#define SCENE_Z         2.0F
#define BEHIND_Z        (-2.0F)
#define UV_SPAN_UNITS   (TEX_SIDE << R3D_TEXTURE_UV_SHIFT) /* one texture side along a triangle's legs */
#define LEFT_COLUMNS    (SIDE / 2)
#define DISTINCT_MORE   3
#define COVERED_MIN     100

typedef struct {
    int16_t positions[SCENE_VERTICES * SCENE_TRIANGLES][3];
    uint8_t colors[SCENE_VERTICES * SCENE_TRIANGLES][3];
    uint16_t triangles[SCENE_TRIANGLES][3];
    int16_t uvs[SCENE_VERTICES * SCENE_TRIANGLES][2];
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

/* The LUVS entry of one material: the layout docs/assets/formats/LUVS.md
 * gives, the sections packed one after another; returns its size. */
static uint32_t
write_luvs(uint8_t* b, uint32_t vertices, uint32_t clusters, uint32_t triangles, const int16_t (*uvs)[2],
           const int16_t (*origins)[2], const uint8_t* materials, const uint8_t* bits) {
    const uint32_t uvs_off = R3D_TEXTURE_UVS_HEADER_SIZE;
    const uint32_t origins_off = uvs_off + (vertices * UV_PAIR_BYTES);
    const uint32_t clusters_off = origins_off + (clusters * UV_PAIR_BYTES);
    const uint32_t triangles_off = clusters_off + clusters;
    const uint32_t bit_bytes =
        (triangles + R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE - 1U) / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE;
    memset(b, 0, LUVS_BYTES_MAX);
    put_u16(b + R3D_TEXTURE_UVS_AT_VERSION, R3D_TEXTURE_UVS_VERSION);
    put_u16(b + R3D_TEXTURE_UVS_AT_MATERIAL_COUNT, 1);
    put_u32(b + R3D_TEXTURE_UVS_AT_VERTEX_COUNT, vertices);
    put_u32(b + R3D_TEXTURE_UVS_AT_CLUSTER_COUNT, clusters);
    put_u32(b + R3D_TEXTURE_UVS_AT_TRIANGLE_COUNT, triangles);
    put_u32(b + R3D_TEXTURE_UVS_AT_UVS_OFF, uvs_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_ORIGINS_OFF, origins_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF, clusters_off);
    put_u32(b + R3D_TEXTURE_UVS_AT_TRIANGLES_OFF, triangles_off);
    memcpy(b + uvs_off, uvs, vertices * UV_PAIR_BYTES);
    memcpy(b + origins_off, origins, clusters * UV_PAIR_BYTES);
    memcpy(b + clusters_off, materials, clusters);
    memcpy(b + triangles_off, bits, bit_bytes);
    return triangles_off + bit_bytes;
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

/* The mesh's UVs: the texture's first side along each triangle's legs. */
static void
leg_uvs(scene_t* s, int triangles) {
    for (int t = 0; t < triangles; t++) {
        const int16_t uv[SCENE_VERTICES][2] = {{0, 0}, {UV_SPAN_UNITS, 0}, {0, UV_SPAN_UNITS}};
        memcpy(s->uvs[t * SCENE_VERTICES], uv, sizeof uv);
        for (int k = 0; k < SCENE_VERTICES; k++) {
            s->triangles[t][k] = (uint16_t)((t * SCENE_VERTICES) + k);
        }
    }
}

/* Builds the one-cluster mesh of `triangles` triangles whose corners are set,
 * with its texturing read from an LUVS entry marking `bits`. */
static void
scene_mesh(scene_t* s, int triangles, uint8_t bits) {
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
    const int16_t origin[1][2] = {{0, 0}};
    const uint8_t material[1] = {0};
    const uint32_t size =
        write_luvs(s->luvs, (uint32_t)vertices, 1, (uint32_t)triangles, s->uvs, origin, material, &bits);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &s->mesh));
    s->mesh.texturing = &s->texturing;
}

static r3d_lens_t
scene_lens(void) {
    const float s = 1.0F / (float)R3D_SUBPIXEL;
    return (r3d_lens_t){{{{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}},
                        {1.0F, -1.0F, 0.0F, 0.0F, 0.5F},
                        SIDE,
                        SIDE,
                        R3D_SNAP_BIAS,
                        R3D_SNAP_BIAS,
                        0.5F / (float)R3D_SUBPIXEL,
                        1.0F,
                        1.0F};
}

/* Draws the scene into target `which`, with its texturing or plain. */
static void
draw_scene(scene_t* s, int which, bool textured) {
    const r3d_lens_t lens = scene_lens();
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
 * READ_TRIANGLES triangles, the last triangle marked and the second
 * cluster without a material. */
static uint32_t
write_read_entry(scene_t* s) {
    const int16_t uvs[READ_VERTICES][2] = {{0, 0}, {0, 0}, {0, 0}, {UV_U_UNITS, UV_V_UNITS}};
    const int16_t origins[READ_CLUSTERS][2] = {{0, 0}, {ORIGIN_U, ORIGIN_V}};
    const uint8_t materials[READ_CLUSTERS] = {0, R3D_TEXTURE_NONE};
    const uint8_t bits[2] = {0, (uint8_t)(1U << (READ_LAST_BIT % R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE))};
    return write_luvs(s->luvs, READ_VERTICES, READ_CLUSTERS, READ_TRIANGLES, uvs, origins, materials, bits);
}

static r3d_lit_mesh_t
read_mesh(void) {
    return (r3d_lit_mesh_t){
        .vertex_count = READ_VERTICES, .cluster_count = READ_CLUSTERS, .triangle_count = READ_TRIANGLES};
}

static void
test_luvs_entry_opens_and_exposes_its_bits_and_uvs(void) {
    scene_t* s = scene_fixture();
    const r3d_lit_mesh_t mesh = read_mesh();
    const uint32_t size = write_read_entry(s);
    TEST_ASSERT_EQUAL(ASSET_OK, open_texturing(s, size, &mesh));
    TEST_ASSERT_EQUAL(1, s->texturing.material_count);
    TEST_ASSERT_EQUAL_PTR(&s->material, s->texturing.materials);
    TEST_ASSERT_EQUAL_UINT8(0, s->texturing.cluster_material[0]);
    TEST_ASSERT_EQUAL_UINT8(R3D_TEXTURE_NONE, s->texturing.cluster_material[1]);
    for (int t = 0; t < READ_TRIANGLES; t++) {
        TEST_ASSERT_EQUAL_MESSAGE(t == READ_LAST_BIT, r3d_texture_triangle_textured(&s->texturing, t),
                                  "a triangle bit");
    }
    /* The origin counts in texture sides, the vertex's UV in 1/16 texel. */
    const r3d_span_uv_t uv = r3d_texture_uv(&s->texturing, &s->material, 1, READ_VERTICES - 1);
    TEST_ASSERT_EQUAL_FLOAT((float)(ORIGIN_U * TEX_SIDE) + ((float)UV_U_UNITS / (1 << R3D_TEXTURE_UV_SHIFT)), uv.u);
    TEST_ASSERT_EQUAL_FLOAT((float)(ORIGIN_V * TEX_SIDE) + ((float)UV_V_UNITS / (1 << R3D_TEXTURE_UV_SHIFT)), uv.v);
}

static void
expect_refused(scene_t* s, uint32_t size, const r3d_lit_mesh_t* mesh, asset_status_t expected, const char* why) {
    memset(&s->texturing, 0xFF, sizeof s->texturing);
    TEST_ASSERT_EQUAL_MESSAGE(expected, open_texturing(s, size, mesh), why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.uvs, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.cluster_material, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.cluster_origin, why);
    TEST_ASSERT_NULL_MESSAGE(s->texturing.triangle_textured, why);
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
test_luvs_entry_must_end_exactly_after_its_triangle_bits(void) {
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
    TEST_ASSERT_NULL(s->texturing.triangle_textured);
}

/* One cluster of two disjoint triangles, the left one in the left half. */
static void
two_triangles(scene_t* s, uint8_t bits) {
    const float left[3][2] = {{2, 4}, {22, 4}, {12, 40}};
    const float right[3][2] = {{26, 4}, {46, 4}, {36, 40}};
    for (int k = 0; k < SCENE_VERTICES; k++) {
        corner(s, k, left[k][0], left[k][1], SCENE_Z);
        corner(s, SCENE_VERTICES + k, right[k][0], right[k][1], SCENE_Z);
    }
    leg_uvs(s, SCENE_TRIANGLES);
    scene_mesh(s, SCENE_TRIANGLES, bits);
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
test_only_marked_triangles_of_a_cluster_draw_textured(void) {
    const uint8_t marks[] = {0x01, 0x02, 0x00};
    for (size_t m = 0; m < sizeof marks; m++) {
        scene_t* s = scene_fixture();
        two_triangles(s, marks[m]);
        clear();
        draw_scene(s, 0, false);
        draw_scene(s, 1, true);
        int left, right;
        differing(&left, &right);
        expect_drawn_textured((marks[m] & 1U) != 0U, left, "the left triangle");
        expect_drawn_textured((marks[m] & 2U) != 0U, right, "the right triangle");
        scene_cleanup();
    }
}

static void
test_near_clipped_textured_triangle_keeps_its_coverage_and_its_pattern(void) {
    scene_t* s = scene_fixture();
    /* Two corners in front, one behind the near plane. */
    corner(s, 0, 8, 20, SCENE_Z);
    corner(s, 1, 40, 20, SCENE_Z);
    corner(s, 2, 0, 0, BEHIND_Z);
    leg_uvs(s, 1);
    scene_mesh(s, 1, 1);
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

void
run_r3d_texture_suite(void) {
    RUN_TEST(test_textured_covers_what_plain_covers);
    RUN_TEST(test_shared_edge_neither_cracks_nor_overlaps);
    RUN_TEST(test_affine_samples_the_texel_under_each_centre);
    RUN_TEST(test_minified_triangle_takes_the_matching_level);
    RUN_TEST(test_texel_at_the_mean_draws_the_vertex_colour);
    RUN_TEST(test_entry_rejects_bad_headers);
    RUN_TEST(test_luvs_entry_opens_and_exposes_its_bits_and_uvs);
    RUN_TEST(test_luvs_entry_is_refused_when_its_counts_do_not_match_the_mesh);
    RUN_TEST(test_luvs_entry_must_end_exactly_after_its_triangle_bits);
    RUN_TEST(test_luvs_cluster_material_past_the_count_is_refused);
    RUN_TEST(test_luvs_names_a_material_the_pack_lacks);
    RUN_TEST(test_only_marked_triangles_of_a_cluster_draw_textured);
    RUN_TEST(test_near_clipped_textured_triangle_keeps_its_coverage_and_its_pattern);
}

SUITE_REGISTER(run_r3d_texture_suite);
#endif
