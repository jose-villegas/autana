/*
 * Portable suite: the asset pack container (asset/asset_pack.h), the lit mesh
 * view built from an entry (render/r3d_lit_mesh.h) and a scene's binding of
 * mesh ids to a pack. Each test builds its own small pack byte by byte, so a
 * check never leans on what the committed pack holds; the last tests open
 * that one, which is the file the host reader and the device partition share.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_pack.h"
#include "asset/asset_store.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_scene.h"

#ifndef DEVICE_BUILD
#include "asset/asset_file.h"
#include "esp_heap_caps.h"
#endif

#define BUFFER_BYTES 512
#define OTHER_TYPE   7u

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

/* The arrays of the one-triangle mesh below: each starts where the last ends,
 * on a 4-byte boundary, after the 44-byte header. */
enum { POSITIONS_AT = 44, COLORS_AT = 64, TRIANGLES_AT = 76, CLUSTERS_AT = 84, NODES_AT = 108, ENTRY_END = 124 };

/* The bytes of that mesh entry; returns its size. */
static uint32_t
make_mesh_entry(uint8_t* entry) {
    memset(entry, 0, 128);
    const uint32_t positions = POSITIONS_AT;
    const uint32_t colors = COLORS_AT;
    const uint32_t triangles = TRIANGLES_AT;
    const uint32_t clusters = CLUSTERS_AT;
    const uint32_t nodes = NODES_AT;
    const uint32_t end = ENTRY_END;
    const uint32_t words[11] = {3, 1, 1, 1, 8, positions, colors, triangles, clusters, nodes, 0};
    for (int i = 0; i < 11; i++) {
        put32(entry + (4 * i), words[i]);
    }
    for (int v = 0; v < 3; v++) {
        put16(entry + positions + (6 * v), v * 10);
        entry[colors + (3 * v)] = (uint8_t)(50 * v);
    }
    for (int v = 0; v < 3; v++) {
        put16(entry + triangles + (2 * v), v);
    }
    put16(entry + clusters + 2, 3);   /* vertex_count */
    put16(entry + clusters + 6, 1);   /* triangle_count */
    put16(entry + clusters + 14, 20); /* hi[0]: lo is the origin */
    put16(entry + nodes + 6, 20);
    entry[nodes + 14] = 1; /* count */
    entry[nodes + 15] = 1; /* leaf */
    return end;
}

/* A pack of one entry named "tri" of `type`, with the CRC set. */
static uint32_t
make_pack(uint8_t* pack, const uint8_t* entry, uint32_t entry_size, uint32_t type) {
    const uint32_t offset = 96; /* 32 header, 48 table, 16 aligned */
    memset(pack, 0, BUFFER_BYTES);
    memcpy(pack, ASSET_PACK_MAGIC, 4);
    put32(pack + 4, ASSET_PACK_VERSION);
    put32(pack + 8, 1);
    memcpy(pack + 32, "tri", 4);
    put32(pack + 32 + 32, type);
    put32(pack + 32 + 36, offset);
    put32(pack + 32 + 40, entry_size);
    put32(pack + 32 + 44, 16);
    memcpy(pack + offset, entry, entry_size);
    const uint32_t total = offset + entry_size;
    put32(pack + 12, total);
    put32(pack + 16, asset_crc32(pack + ASSET_PACK_HEADER_SIZE, total - ASSET_PACK_HEADER_SIZE));
    return total;
}

static void
seal(uint8_t* pack, uint32_t total) {
    put32(pack + 16, asset_crc32(pack + ASSET_PACK_HEADER_SIZE, total - ASSET_PACK_HEADER_SIZE));
}

typedef struct {
    uint8_t* pack;
    uint8_t* entry;
    uint32_t total;
    uint32_t entry_size;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f = {.pack = malloc(BUFFER_BYTES), .entry = malloc(128)};
    TEST_ASSERT_NOT_NULL(f.pack);
    TEST_ASSERT_NOT_NULL(f.entry);
    f.entry_size = make_mesh_entry(f.entry);
    f.total = make_pack(f.pack, f.entry, f.entry_size, R3D_LIT_MESH_ASSET);
    return f;
}

static void
release(fixture_t* f) {
    free(f->entry);
    free(f->pack);
}

static void
test_a_built_pack_opens_and_its_mesh_is_a_view_into_it(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f.pack, f.total));
    TEST_ASSERT_EQUAL_UINT32(1, pack.count);
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(&pack, "tri", &mesh));
    TEST_ASSERT_EQUAL_INT(3, mesh.vertex_count);
    TEST_ASSERT_EQUAL_INT(8, mesh.position_scale);
    TEST_ASSERT_EQUAL_INT16(20, mesh.positions[2][0]);
    TEST_ASSERT_EQUAL_UINT8(100, mesh.colors[2][0]);
    TEST_ASSERT_EQUAL_UINT16(2, mesh.triangles[0][2]);
    TEST_ASSERT_NULL(mesh.face_colors);
    TEST_ASSERT_TRUE_MESSAGE((const uint8_t*)mesh.positions >= f.pack
                                 && (const uint8_t*)mesh.positions < f.pack + f.total,
                             "the view points outside the pack: it was copied");
    release(&f);
}

static void
test_a_pack_larger_than_its_content_opens(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f.pack, BUFFER_BYTES));
    TEST_ASSERT_EQUAL_UINT32(f.total, pack.size);
    release(&f);
}

static void
test_one_changed_byte_fails_the_checksum(void) {
    fixture_t f = fixture();
    f.pack[f.total - 1] ^= 1;
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_CRC, asset_pack_open(&pack, f.pack, f.total));
    TEST_ASSERT_NULL(pack.base);
    release(&f);
}

static void
test_another_format_version_is_refused(void) {
    fixture_t f = fixture();
    put32(f.pack + 4, ASSET_PACK_VERSION + 1);
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, asset_pack_open(&pack, f.pack, f.total));
    release(&f);
}

static void
test_what_is_not_a_pack_is_refused(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, asset_pack_open(&pack, NULL, 0));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TRUNCATED, asset_pack_open(&pack, f.pack, 8));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TRUNCATED, asset_pack_open(&pack, f.pack, f.total - 1));
    f.pack[0] = 'X';
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_MAGIC, asset_pack_open(&pack, f.pack, f.total));
    release(&f);
}

static void
test_an_entry_that_leaves_the_pack_is_refused_even_with_a_good_checksum(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    put32(f.pack + 32 + 40, f.total); /* its size now runs past the end */
    seal(f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, asset_pack_open(&pack, f.pack, f.total));
    put32(f.pack + 32 + 40, 0xFFFFFFF0u); /* a size that wraps when added to the offset */
    seal(f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, asset_pack_open(&pack, f.pack, f.total));
    release(&f);
}

static void
test_a_misaligned_or_table_overlapping_entry_is_refused(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    put32(f.pack + 32 + 36, 98); /* not a multiple of its 16 */
    seal(f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, asset_pack_open(&pack, f.pack, f.total));
    put32(f.pack + 32 + 36, 64); /* aligned, but inside the table */
    seal(f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, asset_pack_open(&pack, f.pack, f.total));
    release(&f);
}

static void
test_a_count_the_table_cannot_hold_is_refused(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    put32(f.pack + 8, 1000);
    seal(f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, asset_pack_open(&pack, f.pack, f.total));
    release(&f);
}

static void
test_a_missing_id_and_a_wrong_type_are_told_apart(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    asset_view_t view;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f.pack, f.total));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, asset_pack_find(&pack, "triangle", R3D_LIT_MESH_ASSET, &view));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, asset_pack_find(&pack, "", R3D_LIT_MESH_ASSET, &view));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TYPE, asset_pack_find(&pack, "tri", OTHER_TYPE, &view));
    TEST_ASSERT_NULL(view.data);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&pack, "tri", R3D_LIT_MESH_ASSET, &view));
    TEST_ASSERT_EQUAL_UINT32(f.entry_size, view.size);
    release(&f);
}

/* A mesh entry whose own offsets or ranges are wrong, in a pack that is not. */
static asset_status_t
open_mesh_with(fixture_t* f, uint32_t at, uint32_t value) {
    put32(f->entry + at, value);
    f->total = make_pack(f->pack, f->entry, f->entry_size, R3D_LIT_MESH_ASSET);
    asset_pack_t pack;
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f->pack, f->total));
    return r3d_lit_mesh_open(&pack, "tri", &mesh);
}

static void
test_a_mesh_array_outside_its_entry_is_refused(void) {
    fixture_t f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, 20, f.entry_size - 4)); /* positions run off the end */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, 20, 46)); /* off a 4-byte boundary */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, 20, 4)); /* inside the header */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, 0, 60000)); /* more vertices than the entry holds */
    release(&f);
}

static void
test_a_cluster_or_node_range_outside_the_mesh_is_refused(void) {
    fixture_t f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, CLUSTERS_AT + 2, 4)); /* four vertices of three */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, 12, 2)); /* two clusters' rows do not fit the entry */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, open_mesh_with(&f, 24, 0)); /* no colours at all */
    release(&f);
}

static r3d_lit_mesh_t scene_mesh;
static const r3d_scene_mesh_t scene_meshes[] = {{"tri", &scene_mesh}, {"gone", &scene_mesh}};
static const r3d_scene_assets_t scene_assets = {.meshes = scene_meshes, .count = 2};

static void
test_a_scene_naming_a_missing_mesh_fails_with_that_id(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f.pack, f.total));
    const char* failed = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, r3d_scene_bind(&pack, &scene_assets, &failed));
    TEST_ASSERT_EQUAL_STRING("gone", failed);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, r3d_scene_bind(NULL, &scene_assets, &failed));
    TEST_ASSERT_EQUAL_STRING("tri", failed);
    const r3d_scene_assets_t only_tri = {.meshes = scene_meshes, .count = 1};
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_scene_bind(&pack, &only_tri, &failed));
    TEST_ASSERT_NULL(failed);
    release(&f);
}

/* What the shipped pack must hold: the file the host reads and the partition
 * the device maps. */
static void
test_the_store_opens_the_shipped_pack_and_it_has_meshes(void) {
    const asset_pack_t* pack = asset_store_pack();
    TEST_ASSERT_NOT_NULL_MESSAGE(pack, "the asset pack did not open: see the log above");
    TEST_ASSERT_GREATER_THAN_UINT32(0, pack->count);
    for (uint32_t i = 0; i < pack->count; i++) {
        const char* name = (const char*)pack->base + ASSET_PACK_HEADER_SIZE + (i * ASSET_PACK_ENTRY_SIZE);
        r3d_lit_mesh_t mesh;
        TEST_ASSERT_EQUAL_INT_MESSAGE(ASSET_OK, r3d_lit_mesh_open(pack, name, &mesh), name);
        TEST_ASSERT_GREATER_THAN_INT(0, mesh.triangle_count);
    }
}

#ifndef DEVICE_BUILD
static void
test_the_host_reader_reads_the_shipped_pack_and_refuses_a_missing_file(void) {
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, asset_file_open("no/such/assets.bin", &pack, &buffer));
    TEST_ASSERT_NULL(buffer);
    const asset_pack_t* shipped = asset_store_pack();
    TEST_ASSERT_NOT_NULL(shipped);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, shipped->base, shipped->size));
    TEST_ASSERT_EQUAL_UINT32(shipped->count, pack.count);
}
#endif

void
suite_asset_pack(void) {
    RUN_TEST(test_a_built_pack_opens_and_its_mesh_is_a_view_into_it);
    RUN_TEST(test_a_pack_larger_than_its_content_opens);
    RUN_TEST(test_one_changed_byte_fails_the_checksum);
    RUN_TEST(test_another_format_version_is_refused);
    RUN_TEST(test_what_is_not_a_pack_is_refused);
    RUN_TEST(test_an_entry_that_leaves_the_pack_is_refused_even_with_a_good_checksum);
    RUN_TEST(test_a_misaligned_or_table_overlapping_entry_is_refused);
    RUN_TEST(test_a_count_the_table_cannot_hold_is_refused);
    RUN_TEST(test_a_missing_id_and_a_wrong_type_are_told_apart);
    RUN_TEST(test_a_mesh_array_outside_its_entry_is_refused);
    RUN_TEST(test_a_cluster_or_node_range_outside_the_mesh_is_refused);
    RUN_TEST(test_a_scene_naming_a_missing_mesh_fails_with_that_id);
    RUN_TEST(test_the_store_opens_the_shipped_pack_and_it_has_meshes);
#ifndef DEVICE_BUILD
    RUN_TEST(test_the_host_reader_reads_the_shipped_pack_and_refuses_a_missing_file);
#endif
}

SUITE_REGISTER(suite_asset_pack);
