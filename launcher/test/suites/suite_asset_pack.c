/*
 * Portable suite: the asset pack container (asset/asset_pack.h), the lit mesh
 * view built from an entry (render/r3d_lit_mesh.h). Each test builds its own
 * small pack byte by byte, so a check never leans on what the tree's bundles
 * hold; on a host the last tests read it back from a file.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_pack.h"
#include "render/r3d_lit_mesh.h"
#include "test_alloc.h"

#ifndef DEVICE_BUILD
#include "asset/asset_file.h"
#include "heap_arena.h"
#endif

#define BUFFER_BYTES 512
#define OTHER_TYPE   7U

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
    void* raw;
    uint8_t* entry;
    uint32_t total;
    uint32_t entry_size;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f = {.entry = malloc(128)};
    f.pack = test_alloc_aligned(BUFFER_BYTES, ASSET_PACK_BASE_ALIGN, &f.raw);
    TEST_ASSERT_NOT_NULL(f.pack);
    TEST_ASSERT_NOT_NULL(f.entry);
    f.entry_size = make_mesh_entry(f.entry);
    f.total = make_pack(f.pack, f.entry, f.entry_size, R3D_LIT_MESH_ASSET);
    return f;
}

static void
release(fixture_t* f) {
    free(f->entry);
    test_free_aligned(f->raw);
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
    put32(f.pack + 32 + 40, 0xFFFFFFF0U); /* a size that wraps when added to the offset */
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
}

/* Inside its range, a field holds a value no mesh can have. */
static void
test_a_mesh_without_one_colour_source_a_scale_clusters_or_nodes_is_a_format_error(void) {
    fixture_t f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_mesh_with(&f, 24, 0)); /* no colours at all */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_mesh_with(&f, 16, 0)); /* no position scale */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_mesh_with(&f, 8, 0)); /* no clusters */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_mesh_with(&f, 12, 0)); /* no nodes */
    release(&f);
    f = fixture();
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, open_mesh_with(&f, 40, COLORS_AT)); /* both colour sources */
    release(&f);
}

static void
test_an_entry_row_is_read_by_its_index_and_an_index_past_the_table_is_not_found(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    asset_entry_t entry;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f.pack, f.total));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_entry(&pack, 0, &entry));
    TEST_ASSERT_EQUAL_STRING("tri", entry.name);
    TEST_ASSERT_EQUAL_UINT32(R3D_LIT_MESH_ASSET, entry.type);
    TEST_ASSERT_EQUAL_UINT32(f.entry_size, entry.view.size);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, asset_pack_entry(&pack, 1, &entry));
    TEST_ASSERT_EQUAL_UINT32(f.total, asset_pack_total_size(f.pack, ASSET_PACK_HEADER_SIZE));
    TEST_ASSERT_EQUAL_UINT32(0, asset_pack_total_size(f.entry, ASSET_PACK_HEADER_SIZE));
    release(&f);
}

static void
test_a_base_that_is_not_16_byte_aligned_is_refused(void) {
    fixture_t f = fixture();
    void* raw;
    uint8_t* aligned = test_alloc_aligned(BUFFER_BYTES + 16, ASSET_PACK_BASE_ALIGN, &raw);
    TEST_ASSERT_NOT_NULL(aligned);
    memcpy(aligned + 8, f.pack, f.total);
    asset_pack_t pack;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, asset_pack_open(&pack, aligned + 8, f.total));
    memcpy(aligned, f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, aligned, f.total));
    test_free_aligned(raw);
    release(&f);
}

static void
test_a_header_with_reserved_bytes_in_use_is_refused(void) {
    fixture_t f = fixture();
    asset_pack_t pack;
    f.pack[24] = 1;
    seal(f.pack, f.total);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_SIZE, asset_pack_open(&pack, f.pack, f.total));
    release(&f);
}

static void
test_an_inner_node_whose_children_are_not_after_it_is_refused(void) {
    fixture_t f = fixture();
    /* The only node is a leaf; making it an inner node over itself would loop a walk. */
    f.entry[NODES_AT + 15] = 0;
    f.entry[NODES_AT + 14] = 1;
    put16(f.entry + NODES_AT + 12, 0);
    f.total = make_pack(f.pack, f.entry, f.entry_size, R3D_LIT_MESH_ASSET);
    asset_pack_t pack;
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, f.pack, f.total));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, r3d_lit_mesh_open(&pack, "tri", &mesh));
    release(&f);
}

#ifndef DEVICE_BUILD
#define PACK_FILE "suite_asset_pack.apak"

/* The fixture's pack, written where the host reader reads it. */
static void
write_pack_file(const fixture_t* f) {
    FILE* file = fopen(PACK_FILE, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(f->total, fwrite(f->pack, 1, f->total, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void
test_the_host_reader_reads_a_pack_file_and_refuses_a_missing_one(void) {
    fixture_t f = fixture();
    write_pack_file(&f);
    asset_pack_t pack;
    void* buffer = NULL;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NO_PACK, asset_file_open("no/such/bundle.apak", &pack, &buffer));
    TEST_ASSERT_NULL(buffer);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(PACK_FILE, &pack, &buffer));
    r3d_lit_mesh_t mesh;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(&pack, "tri", &mesh));
    asset_file_release(buffer);
    TEST_ASSERT_EQUAL_INT(0, remove(PACK_FILE));
    release(&f);
}

static void
test_the_host_reader_holds_the_pack_outside_the_modelled_heap(void) {
    fixture_t f = fixture();
    write_pack_file(&f);
    asset_pack_t pack;
    void* buffer = NULL;
    size_t blocks_before;
    size_t bytes_before;
    heap_arena_snapshot(&blocks_before, &bytes_before);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(PACK_FILE, &pack, &buffer));
    size_t blocks_after;
    size_t bytes_after;
    heap_arena_snapshot(&blocks_after, &bytes_after);
    asset_file_release(buffer);
    TEST_ASSERT_EQUAL_INT(0, remove(PACK_FILE));
    release(&f);
    TEST_ASSERT_EQUAL_UINT(bytes_before, bytes_after);
    TEST_ASSERT_EQUAL_UINT(blocks_before, blocks_after);
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
    RUN_TEST(test_a_mesh_without_one_colour_source_a_scale_clusters_or_nodes_is_a_format_error);
    RUN_TEST(test_an_entry_row_is_read_by_its_index_and_an_index_past_the_table_is_not_found);
    RUN_TEST(test_a_base_that_is_not_16_byte_aligned_is_refused);
    RUN_TEST(test_a_header_with_reserved_bytes_in_use_is_refused);
    RUN_TEST(test_an_inner_node_whose_children_are_not_after_it_is_refused);
#ifndef DEVICE_BUILD
    RUN_TEST(test_the_host_reader_reads_a_pack_file_and_refuses_a_missing_one);
    RUN_TEST(test_the_host_reader_holds_the_pack_outside_the_modelled_heap);
#endif
}

SUITE_REGISTER(suite_asset_pack);
