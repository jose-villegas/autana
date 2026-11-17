#include <float.h>
#include <math.h>
#include <stdlib.h>
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
    TEST_ASSERT_FLOAT_WITHIN(0.01F, CONTACT_RADIUS_FRACTION, contacts.radius);
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
    TEST_ASSERT_FLOAT_WITHIN(0.01F, CONTACT_RADIUS_FRACTION * (10 - 1.0F / CONTACT_TEST_JOINTS), contacts.radius);
    free(inverse);
    free(joints);
}

static int
pack_mask_cell(const contact_mask_t* mask, vec3f_t point) {
    const int x = mathi_clamp((int)((point.x - mask->origin.x) * mask->side / mask->u.x), 0, mask->side - 1);
    const int y = mathi_clamp((int)((point.z - mask->origin.z) * mask->side / mask->v.z), 0, mask->side - 1);
    return mask->coverage[y * mask->side + x];
}

static void
assert_feet_darker_than_head(const contact_mask_t* mask, const r3d_lit_mesh_t* mesh, const r3d_skin_t* skin,
                             const skin_contacts_t* contacts) {
    int highest = 0, feet = 0;
    for (int v = 1; v < mesh->vertex_count; v++) {
        if (mesh->positions[v][1] > mesh->positions[highest][1]) {
            highest = v;
        }
    }
    const int16_t* p = mesh->positions[highest];
    const vec3f_t head = {(float)p[0] / mesh->position_scale, (float)p[1] / mesh->position_scale,
                          (float)p[2] / mesh->position_scale};
    for (int i = 0; i < contacts->count; i++) {
        feet += pack_mask_cell(mask, skin_contact_bind_position(skin, contacts->joints[i]));
    }
    TEST_ASSERT_GREATER_THAN_INT(pack_mask_cell(mask, head) * contacts->count, feet);
}

static void
assert_pack_mask(const r3d_lit_mesh_t* mesh, const anim_skeleton_t* skeleton, const r3d_skin_t* skin,
                 const skin_contacts_t* contacts) {
    const int joints = skin->joint_count;
    transformf_t* local = malloc((size_t)joints * sizeof *local);
    mat4f_t* model = malloc((size_t)joints * sizeof *model);
    mat4f_t* palette = malloc((size_t)joints * sizeof *palette);
    void* storage = malloc(r3d_skin_output_bytes(mesh));
    contact_mask_t mask = {.coverage = malloc(CONTACT_MASK_BYTES)};
    TEST_ASSERT_NOT_NULL(local);
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_NOT_NULL(palette);
    TEST_ASSERT_NOT_NULL(storage);
    TEST_ASSERT_NOT_NULL(mask.coverage);
    anim_pose_t pose = {.local = local, .joint_count = joints};
    anim_pose_rest(skeleton, &pose);
    anim_pose_model(skeleton, &pose, model);
    r3d_skin_palette(skin, model, mesh->position_scale, palette);
    r3d_skin_output_t output;
    TEST_ASSERT_TRUE(r3d_skin_output_init(mesh, storage, r3d_skin_output_bytes(mesh), &output));
    r3d_skin_clusters(skin, mesh, palette, 0, mesh->cluster_count, &output);
    r3d_skin_refit_nodes(&output);
    const box3f_t box = r3d_lit_mesh_world_bounds(&output.mesh, NULL);
    contact_mask_build(&mask, &output.mesh, mat4f_identity(), box, box.lo.y, contacts->lift, 1);
    unsigned total = 0, left = 0, right = 0;
    for (int y = 0; y < mask.side; y++) {
        int first = mask.side, last = -1;
        for (int x = 0; x < mask.side; x++) {
            const unsigned c = mask.coverage[y * mask.side + x];
            total += c;
            if (c > 0) {
                first = mathi_min(first, x);
                last = x;
            }
            if (x < mask.side / 2) {
                left += c;
            } else {
                right += c;
            }
        }
        if (last >= first) {
            TEST_ASSERT_INT_WITHIN(2, mask.side - 1, first + last);
        }
    }
    assert_feet_darker_than_head(&mask, &output.mesh, skin, contacts);
    TEST_ASSERT_GREATER_THAN_UINT(0, total);
    TEST_ASSERT_UINT_WITHIN(total / mask.side, left, right);
    free(mask.coverage);
    free(storage);
    free(palette);
    free(model);
    free(local);
}

static void
assert_pack_shape(const asset_pack_t* pack, const anim_skeleton_t* skeleton, const r3d_skin_t* skin,
                  const skin_contacts_t* contacts) {
    for (uint32_t i = 0; i < pack->count; i++) {
        asset_entry_t entry;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, asset_pack_entry(pack, i, &entry));
        if (entry.type != R3D_LIT_MESH_ASSET) {
            continue;
        }
        r3d_lit_mesh_t mesh;
        TEST_ASSERT_EQUAL_INT(ASSET_OK, r3d_lit_mesh_open(pack, entry.name, &mesh));
        if (mesh.vertex_count == (int)skin->vertex_count) {
            assert_pack_mask(&mesh, skeleton, skin, contacts);
            return;
        }
    }
    TEST_FAIL_MESSAGE("skin mesh missing");
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
        TEST_ASSERT_LESS_OR_EQUAL_FLOAT(lowest + CONTACT_JOINT_BAND * (high - low), contacts.rest_y[i]);
        TEST_ASSERT_FLOAT_WITHIN(0.002F, 0.027F, contacts.rest_y[i]);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.002F, 0.133F, contacts.radius);
    TEST_ASSERT_FLOAT_WITHIN(0.01F, 0.18F, contacts.lift);
    assert_pack_shape(pack, &skeleton, &skin, &contacts);
    asset_store_release(loaded);
}

void
suite_skin_contact(void) {
    RUN_TEST(test_contact_leaves_sorted_and_limited);
    RUN_TEST(test_pack_contact_leaves);
}

SUITE_REGISTER(suite_skin_contact);
SUITE_READS(suite_skin_contact, "capybara");
