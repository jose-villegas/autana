#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "anim/pose/anim_pose.h"
#include "asset/asset_store.h"
#include "math/scalar/mathi.h"
#include "render/contact/contact_mask.h"
#include "render/skin/r3d_skinning.h"
#include "scene/skin/skin_contact.h"
#include "suites.h"
#include "unity.h"

enum { CONTACT_TEST_JOINTS = SKIN_CONTACTS_MAX + 4 };

static void
test_contact_leaves_sorted_and_limited(void) {
    anim_skeleton_joint_t* joints = calloc(CONTACT_TEST_JOINTS, sizeof *joints);
    float (*inverse)[R3D_SKIN_MATRIX_WIDTH] = calloc(CONTACT_TEST_JOINTS, sizeof *inverse);
    TEST_ASSERT_NOT_NULL(joints);
    TEST_ASSERT_NOT_NULL(inverse);
    for (int j = 0; j < CONTACT_TEST_JOINTS; j++) {
        joints[j].parent = j == 0 ? ANIM_SKELETON_ROOT : 0;
        inverse[j][0] = inverse[j][5] = inverse[j][10] = 1;
        inverse[j][3] = -(float)j;
        inverse[j][7] = j == 0 ? -10 : -(float)j / CONTACT_TEST_JOINTS;
    }
    anim_skeleton_t skeleton = {.joint_count = CONTACT_TEST_JOINTS, .joints = joints};
    r3d_skin_t skin = {.joint_count = CONTACT_TEST_JOINTS,
                       .inverse_binds = (const float (*)[R3D_SKIN_MATRIX_WIDTH])inverse};
    skin_contacts_t contacts;
    TEST_ASSERT_EQUAL_INT(SKIN_CONTACTS_MAX, skin_contacts_find(&skeleton, &skin, &contacts));
    for (int i = 0; i < contacts.count; i++) {
        TEST_ASSERT_EQUAL_INT(i + 1, contacts.joints[i]);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01F, SKIN_CONTACT_RADIUS_FRACTION, contacts.radius);
    for (int j = 1; j < CONTACT_TEST_JOINTS; j++) {
        inverse[j][7] = -(float)(CONTACT_TEST_JOINTS - j) / CONTACT_TEST_JOINTS;
    }
    TEST_ASSERT_EQUAL_INT(SKIN_CONTACTS_MAX, skin_contacts_find(&skeleton, &skin, &contacts));
    for (int i = 0; i < contacts.count; i++) {
        TEST_ASSERT_EQUAL_INT(CONTACT_TEST_JOINTS - 1 - i, contacts.joints[i]);
    }
    inverse[1][7] = -1.0F / CONTACT_TEST_JOINTS;
    skeleton.joint_count = skin.joint_count = 2;
    TEST_ASSERT_EQUAL_INT(1, skin_contacts_find(&skeleton, &skin, &contacts));
    TEST_ASSERT_FLOAT_WITHIN(0.01F, SKIN_CONTACT_RADIUS_FRACTION * (10 - 1.0F / CONTACT_TEST_JOINTS), contacts.radius);
    free(inverse);
    free(joints);
}

static void
test_coincident_contacts_use_positive_spacing_or_height(void) {
    anim_skeleton_joint_t joints[3] = {{.parent = ANIM_SKELETON_ROOT}, {.parent = 0}, {.parent = 0}};
    float inverse[3][R3D_SKIN_MATRIX_WIDTH] = {{0}};
    for (int j = 0; j < 3; j++) {
        inverse[j][0] = inverse[j][5] = inverse[j][10] = 1;
    }
    inverse[0][7] = -2;
    const anim_skeleton_t skeleton = {.joint_count = 3, .joints = joints};
    const r3d_skin_t skin = {.joint_count = 3, .inverse_binds = (const float (*)[R3D_SKIN_MATRIX_WIDTH])inverse};
    skin_contacts_t contacts;
    TEST_ASSERT_EQUAL_INT(2, skin_contacts_find(&skeleton, &skin, &contacts));
    TEST_ASSERT_EQUAL_FLOAT(2 * SKIN_CONTACT_RADIUS_FRACTION, contacts.radius);
    inverse[2][3] = -1;
    TEST_ASSERT_EQUAL_INT(2, skin_contacts_find(&skeleton, &skin, &contacts));
    TEST_ASSERT_EQUAL_FLOAT(SKIN_CONTACT_RADIUS_FRACTION, contacts.radius);
}

static void
test_pack_contact_leaves(void) {
    const handle_t loaded = asset_store_load("capybara");
    const asset_pack_t* pack = asset_store_loaded(loaded);
    TEST_ASSERT_NOT_NULL(pack);
    anim_skeleton_t skeleton = {0};
    r3d_skin_t skin = {0};
    for (uint32_t i = 0; i < pack->count; i++) {
        asset_entry_t entry;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_entry(pack, i, &entry));
        if (entry.type == ANIM_SKELETON_ASSET) {
            TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_skeleton_open(pack, entry.name, &skeleton));
        }
        if (entry.type == R3D_SKIN_ASSET) {
            TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_skin_open(pack, entry.name, &skin));
        }
    }
    skin_contacts_t contacts;
    TEST_ASSERT_EQUAL_INT(4, skin_contacts_find(&skeleton, &skin, &contacts));
    float lowest = FLT_MAX, low = FLT_MAX, high = -FLT_MAX;
    for (int j = 0; j < skin.joint_count; j++) {
        const float y = skin_contact_bind_position(&skin, j).y;
        low = fminf(low, y);
        high = fmaxf(high, y);
        bool leaf = true;
        for (int k = 0; k < skin.joint_count; k++) {
            leaf &= skeleton.joints[k].parent != j;
        }
        if (leaf) {
            lowest = fminf(lowest, y);
        }
    }
    for (int i = 0; i < contacts.count; i++) {
        for (int j = 0; j < skeleton.joint_count; j++) {
            TEST_ASSERT_NOT_EQUAL(contacts.joints[i], skeleton.joints[j].parent);
        }
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(lowest + SKIN_CONTACT_JOINT_BAND * (high - low),
                                        skin_contact_bind_position(&skin, contacts.joints[i]).y);
        const char* path = skeleton.paths + skeleton.joints[contacts.joints[i]].path;
        const char* joint = strrchr(path, '/');
        joint = joint == NULL ? path : joint + 1;
        TEST_ASSERT_TRUE(strcmp(joint, "toes.l") == 0 || strcmp(joint, "toes.r") == 0 || strcmp(joint, "hand.l") == 0
                         || strcmp(joint, "hand.r") == 0);
        TEST_ASSERT_FLOAT_WITHIN(0.002F, 0.027F, skin_contact_bind_position(&skin, contacts.joints[i]).y);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.002F, 0.133F, contacts.radius);
    asset_store_release(loaded);
}

void
suite_skin_contact(void) {
    RUN_TEST(test_contact_leaves_sorted_and_limited);
    RUN_TEST(test_pack_contact_leaves);
    RUN_TEST(test_coincident_contacts_use_positive_spacing_or_height);
}

SUITE_REGISTER(suite_skin_contact);
SUITE_READS(suite_skin_contact, "capybara");
