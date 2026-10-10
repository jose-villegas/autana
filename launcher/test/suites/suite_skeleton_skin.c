/* Mapped SKEL/SKIN views checked against Python-written probe entries. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_skeleton.h"
#include "asset/asset_bytes.h"
#include "render/r3d_skin.h"
#include "suites.h"
#include "test_pack.h"
#include "unity.h"
#ifndef DEVICE_BUILD
#include "asset/asset_file.h"

typedef struct {
    asset_pack_t pack;
    void* buffer;
    asset_view_t skeleton;
    asset_view_t skin;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f = {0};
    const char* path = getenv("AUTANA_SKIN_PROBE");
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_file_open(path, &f.pack, &f.buffer));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&f.pack, "armature", ANIM_SKELETON_ASSET, &f.skeleton));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_find(&f.pack, "probe.skin", R3D_SKIN_ASSET, &f.skin));
    return f;
}

static void
test_python_probe_views(void) {
    fixture_t f = fixture();
    anim_skeleton_t skeleton;
    r3d_skin_t skin;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_skeleton_open(&f.pack, "armature", &skeleton));
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_skin_open(&f.pack, "probe.skin", &skin));
    TEST_ASSERT_EQUAL_INT(4, skeleton.joint_count);
    TEST_ASSERT_EQUAL_INT(ANIM_SKELETON_ROOT, skeleton.joints[0].parent);
    TEST_ASSERT_EQUAL_INT(ANIM_SKELETON_ROOT, skeleton.joints[1].parent);
    TEST_ASSERT_EQUAL_INT(0, skeleton.joints[2].parent);
    TEST_ASSERT_EQUAL_INT(1, skeleton.joints[3].parent);
    TEST_ASSERT_EQUAL_STRING("a/child", skeleton.paths + skeleton.joints[2].path);
    TEST_ASSERT_EQUAL_INT(skeleton.joint_count, skin.joint_count);
    TEST_ASSERT_EQUAL_INT(R3D_SKIN_INFLUENCES_MAX, skin.influences);
    TEST_ASSERT_EQUAL_INT(3, skin.vertex_count);
    TEST_ASSERT_EQUAL_FLOAT(1, skin.inverse_binds[0][0]);
    TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_skin_open(&f.pack, "probe2.skin", &skin));
    TEST_ASSERT_EQUAL_INT(R3D_SKIN_INFLUENCES_MIN, skin.influences);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_NOT_FOUND, anim_skeleton_open(&f.pack, "missing", &skeleton));
    TEST_ASSERT_NULL(skeleton.rest);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_TYPE, r3d_skin_open(&f.pack, "armature", &skin));
    TEST_ASSERT_NULL(skin.vertices);
    asset_file_release(f.buffer);
}

typedef struct {
    uint32_t at;
    uint32_t value;
    uint8_t width;
    asset_status_t want;
} mutation_t;

static void
check_mutation(const fixture_t* f, asset_view_t entry, const char* id, mutation_t change, int skin) {
    uint8_t* bytes = (uint8_t*)(uintptr_t)entry.data;
    uint32_t saved = 0;
    memcpy(&saved, bytes + change.at, change.width);
    memcpy(bytes + change.at, &change.value, change.width);
    anim_skeleton_t skeleton;
    r3d_skin_t mesh_skin;
    const asset_status_t status =
        skin ? r3d_skin_open(&f->pack, id, &mesh_skin) : anim_skeleton_open(&f->pack, id, &skeleton);
    memcpy(bytes + change.at, &saved, change.width);
    TEST_ASSERT_EQUAL_INT(change.want, status);
    if (skin) {
        TEST_ASSERT_NULL(mesh_skin.vertices);
    } else {
        TEST_ASSERT_NULL(skeleton.rest);
    }
}

static void
test_skeleton_open_refusals(void) {
    fixture_t f = fixture();
    const mutation_t changes[] = {
        {ANIM_SKELETON_AT_VERSION, 2, 2, ASSET_ERR_VERSION},
        {ANIM_SKELETON_AT_COUNT, 0, 1, ASSET_ERR_FORMAT},
        {ANIM_SKELETON_AT_COUNT, 255, 1, ASSET_ERR_FORMAT},
        {ANIM_SKELETON_AT_PAD, 1, 1, ASSET_ERR_FORMAT},
        {ANIM_SKELETON_AT_STRINGS, 17, 4, ASSET_ERR_BOUNDS},
        {ANIM_SKELETON_AT_STRINGS, UINT32_MAX, 4, ASSET_ERR_BOUNDS},
        {ANIM_SKELETON_AT_STRINGS_SIZE, UINT32_MAX, 4, ASSET_ERR_BOUNDS},
        {ANIM_SKELETON_AT_REST, 0, 4, ASSET_ERR_BOUNDS},
        {ANIM_SKELETON_AT_REST, 17, 4, ASSET_ERR_BOUNDS},
        {ANIM_SKELETON_HEADER_SIZE, UINT16_MAX, 2, ASSET_ERR_FORMAT},
        {ANIM_SKELETON_HEADER_SIZE + offsetof(anim_skeleton_joint_t, parent), 0, 1, ASSET_ERR_FORMAT},
        {ANIM_SKELETON_HEADER_SIZE + offsetof(anim_skeleton_joint_t, pad), 1, 1, ASSET_ERR_FORMAT},
        {ANIM_SKELETON_HEADER_SIZE + ANIM_SKELETON_ROW_SIZE, 0, 2, ASSET_ERR_FORMAT},
    };
    for (size_t i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        check_mutation(&f, f.skeleton, "armature", changes[i], 0);
    }
    const uint32_t rest = asset_read_u32(f.skeleton.data + ANIM_SKELETON_AT_REST);
    const uint32_t strings = asset_read_u32(f.skeleton.data + ANIM_SKELETON_AT_STRINGS);
    const float nonfinite = INFINITY;
    uint32_t bits;
    memcpy(&bits, &nonfinite, sizeof bits);
    check_mutation(&f, f.skeleton, "armature", (mutation_t){rest, bits, sizeof bits, ASSET_ERR_FORMAT}, 0);
    check_mutation(&f, f.skeleton, "armature", (mutation_t){strings, 0, 1, ASSET_ERR_FORMAT}, 0);
    check_mutation(&f, f.skeleton, "armature", (mutation_t){f.skeleton.size - 1, 'x', 1, ASSET_ERR_FORMAT}, 0);
    asset_file_release(f.buffer);
}

static void
test_truncated_entries(void) {
    fixture_t f = fixture();
    const asset_view_t entries[] = {f.skeleton, f.skin};
    const uint32_t kinds[] = {ANIM_SKELETON_ASSET, R3D_SKIN_ASSET};
    for (size_t index = 0; index < sizeof entries / sizeof entries[0]; index++) {
        const asset_view_t entry = entries[index];
        const uint32_t sizes[] = {0, ANIM_SKELETON_HEADER_SIZE - 1, entry.size - 1};
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            uint8_t* buffer = malloc(f.pack.size);
            TEST_ASSERT_NOT_NULL(buffer);
            test_pack_t writer = test_pack_begin(buffer, f.pack.size, 1);
            uint8_t* bytes = test_pack_add(&writer, "short", kinds[index], sizes[i]);
            memcpy(bytes, entry.data, sizes[i]);
            const uint32_t size = test_pack_finish(&writer);
            asset_pack_t pack;
            TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_open(&pack, buffer, size));
            anim_skeleton_t skeleton;
            r3d_skin_t skin;
            const asset_status_t status =
                index == 0 ? anim_skeleton_open(&pack, "short", &skeleton) : r3d_skin_open(&pack, "short", &skin);
            free(buffer);
            TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, status);
        }
    }
    asset_file_release(f.buffer);
}

static void
test_skeleton_unit_boundary(void) {
    fixture_t f = fixture();
    const uint32_t rest = asset_read_u32(f.skeleton.data + ANIM_SKELETON_AT_REST);
    const uint32_t at = rest + (ANIM_SKELETON_ROTATION + ANIM_SKELETON_ROTATION_WIDTH - 1) * sizeof(float);
    uint8_t* bytes = (uint8_t*)(uintptr_t)f.skeleton.data;
    for (int sign = -1; sign <= 1; sign += 2) {
        const float boundary = sqrtf(1.0F + ((float)sign * ANIM_SKELETON_UNIT_TOLERANCE));
        const float candidates[] = {nextafterf(boundary, 0), boundary, nextafterf(boundary, INFINITY)};
        int accepted = 0;
        int rejected = 0;
        for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
            const float q = candidates[i];
            memcpy(bytes + at, &q, sizeof q);
            const float norm = q * q;
            const asset_status_t want =
                fabsf(1.0F - norm) <= ANIM_SKELETON_UNIT_TOLERANCE ? ASSET_OK : ASSET_ERR_FORMAT;
            anim_skeleton_t skeleton;
            TEST_ASSERT_EQUAL_INT(want, anim_skeleton_open(&f.pack, "armature", &skeleton));
            accepted += want == ASSET_OK;
            rejected += want == ASSET_ERR_FORMAT;
        }
        TEST_ASSERT_GREATER_THAN_INT(0, accepted);
        TEST_ASSERT_GREATER_THAN_INT(0, rejected);
    }
    asset_file_release(f.buffer);
}

static void
test_skin_open_refusals(void) {
    fixture_t f = fixture();
    const mutation_t changes[] = {
        {R3D_SKIN_AT_VERSION, 2, 2, ASSET_ERR_VERSION},          {R3D_SKIN_AT_JOINTS, 0, 1, ASSET_ERR_FORMAT},
        {R3D_SKIN_AT_JOINTS, 255, 1, ASSET_ERR_FORMAT},          {R3D_SKIN_AT_INFLUENCES, 3, 1, ASSET_ERR_FORMAT},
        {R3D_SKIN_AT_VERTICES, UINT32_MAX, 4, ASSET_ERR_BOUNDS}, {R3D_SKIN_AT_INVERSE, 0, 4, ASSET_ERR_BOUNDS},
        {R3D_SKIN_AT_INVERSE, 17, 4, ASSET_ERR_BOUNDS},          {R3D_SKIN_AT_RECORDS, 0, 4, ASSET_ERR_BOUNDS},
        {R3D_SKIN_AT_RECORDS, 17, 4, ASSET_ERR_BOUNDS},
    };
    for (size_t i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        check_mutation(&f, f.skin, "probe.skin", changes[i], 1);
    }
    const uint32_t records = asset_read_u32(f.skin.data + R3D_SKIN_AT_RECORDS);
    check_mutation(&f, f.skin, "probe.skin", (mutation_t){records, ANIM_SKELETON_JOINT_MAX, 1, ASSET_ERR_FORMAT}, 1);
    check_mutation(&f, f.skin, "probe.skin", (mutation_t){records + R3D_SKIN_INFLUENCES_MAX, 0, 1, ASSET_ERR_FORMAT},
                   1);
    check_mutation(&f, f.skin, "probe.skin",
                   (mutation_t){records + 2 * R3D_SKIN_INFLUENCES_MAX, 128, 1, ASSET_ERR_FORMAT}, 1);
    check_mutation(&f, f.skin, "probe.skin",
                   (mutation_t){records + 2 * R3D_SKIN_INFLUENCES_MAX, 0, R3D_SKIN_RECORD_TAIL, ASSET_ERR_FORMAT}, 1);
    const uint32_t stride = 2 * R3D_SKIN_INFLUENCES_MAX + R3D_SKIN_RECORD_TAIL;
    check_mutation(&f, f.skin, "probe.skin", (mutation_t){records + stride - 1, 1, 1, ASSET_ERR_FORMAT}, 1);
    const float nonfinite = NAN;
    uint32_t bits;
    memcpy(&bits, &nonfinite, sizeof bits);
    const uint32_t inverse = asset_read_u32(f.skin.data + R3D_SKIN_AT_INVERSE);
    check_mutation(&f, f.skin, "probe.skin", (mutation_t){inverse, bits, sizeof bits, ASSET_ERR_FORMAT}, 1);
    asset_file_release(f.buffer);
}
#endif

void
suite_skeleton_skin(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_python_probe_views);
    RUN_TEST(test_truncated_entries);
    RUN_TEST(test_skeleton_open_refusals);
    RUN_TEST(test_skeleton_unit_boundary);
    RUN_TEST(test_skin_open_refusals);
#endif
}

SUITE_REGISTER(suite_skeleton_skin);
