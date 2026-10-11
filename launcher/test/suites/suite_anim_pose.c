#include <stdlib.h>
#include <string.h>
#include "anim/pose/anim_pose.h"
#include "suites.h"
#include "test_anim_tracks.h"
#include "unity.h"

enum { JOINTS = 3, BINDINGS = 3, ENTRY_BYTES = 512, TIMES = 360, VALUES = 364 };

static const anim_skeleton_joint_t JOINT_ROWS[] = {{0, ANIM_SKELETON_ROOT, 0}, {2, 0, 0}, {10, ANIM_SKELETON_ROOT, 0}};
static const float REST[][ANIM_SKELETON_REST_WIDTH] = {
    {2, 0, 0, 0, 0, 0, 1, 2, 2, 2},
    {0, 3, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 4, 0, 0, 0, 1, 1, 1, 1},
};
static const anim_skeleton_t SKELETON = {JOINTS, JOINT_ROWS, REST, "a\0a/child\0b"};

typedef struct {
    transformf_t* local;
    mat4f_t* model;
    anim_pose_t pose;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f = {.local = calloc(JOINTS, sizeof *f.local), .model = calloc(JOINTS, sizeof *f.model)};
    TEST_ASSERT_NOT_NULL(f.local);
    TEST_ASSERT_NOT_NULL(f.model);
    f.pose = (anim_pose_t){f.local, JOINTS};
    return f;
}

static void
release(fixture_t* f) {
    free(f->model);
    free(f->local);
}

static void
test_rest_and_multiple_roots(void) {
    fixture_t f = fixture();
    anim_pose_rest(&SKELETON, &f.pose);
    TEST_ASSERT_EQUAL_FLOAT(2, f.local[0].position.x);
    TEST_ASSERT_EQUAL_FLOAT(1, f.local[1].rotation.w);
    anim_pose_model(&SKELETON, &f.pose, f.model);
    TEST_ASSERT_EQUAL_FLOAT(2, f.model[1].m[0][3]);
    TEST_ASSERT_EQUAL_FLOAT(6, f.model[1].m[1][3]);
    TEST_ASSERT_EQUAL_FLOAT(4, f.model[2].m[2][3]);
    TEST_ASSERT_EQUAL_FLOAT(0, f.model[2].m[0][3]);
    release(&f);
}

static void
test_transition_blend_endpoints_midpoint_and_hemisphere(void) {
    fixture_t a = fixture(), b = fixture(), out = fixture();
    anim_pose_rest(&SKELETON, &a.pose);
    anim_pose_rest(&SKELETON, &b.pose);
    transformf_set_position(&b.local[0], (vec3f_t){6, 0, 0});
    transformf_set_scale(&b.local[0], (vec3f_t){4, 4, 4});
    transformf_set_rotation(&b.local[0], (quatf_t){0, 0, 1, 0});

    static const struct {
        float weight, position, z, w;
    } ENDPOINTS[] = {{0, 2, 0, 1}, {1, 6, 1, 0}};

    for (size_t i = 0; i < sizeof ENDPOINTS / sizeof ENDPOINTS[0]; i++) {
        anim_pose_blend(&a.pose, &b.pose, ENDPOINTS[i].weight, &out.pose);
        TEST_ASSERT_EQUAL_FLOAT(ENDPOINTS[i].position, out.local[0].position.x);
        TEST_ASSERT_EQUAL_FLOAT(ENDPOINTS[i].z, out.local[0].rotation.z);
        TEST_ASSERT_EQUAL_FLOAT(ENDPOINTS[i].w, out.local[0].rotation.w);
    }
    anim_pose_blend(&a.pose, &b.pose, 0.5F, &out.pose);
    TEST_ASSERT_EQUAL_FLOAT(4, out.local[0].position.x);
    TEST_ASSERT_EQUAL_FLOAT(3, out.local[0].scale.x);
    const vec3f_t turned = quatf_rotate(out.local[0].rotation, (vec3f_t){1, 0, 0});
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, 0, turned.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5F, 1, turned.y);
    transformf_set_rotation(&b.local[0], (quatf_t){0, 0, 0, -1});
    anim_pose_blend(&a.pose, &b.pose, 0.5F, &a.pose);
    TEST_ASSERT_EQUAL_FLOAT(1, a.local[0].rotation.w);
    TEST_ASSERT_FALSE(a.local[0].cached);
    release(&out);
    release(&b);
    release(&a);
}

static void
test_rotation_blend_preserves_float_bits(void) {
    fixture_t a = fixture(), b = fixture(), out = fixture();
    anim_pose_rest(&SKELETON, &a.pose);
    anim_pose_rest(&SKELETON, &b.pose);
    const quatf_t av = quatf_normalize((quatf_t){1, 2, 3, 4});
    static const quatf_t targets[] = {{4, -3, -2, 1}, {-4, 3, 2, -1}, {4, 3, -2, -1}};
    static const float weights[] = {-1, 0, 1.0F / 256, 0.25F, 0.5F, 1, 2};
    transformf_set_rotation(&a.local[0], av);
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        const quatf_t bv = quatf_normalize(targets[i]);
        transformf_set_rotation(&b.local[0], bv);
        for (size_t j = 0; j < sizeof weights / sizeof weights[0]; j++) {
            const float weight = mathf_clamp(weights[j], 0, 1);
            const float dot = av.x * bv.x + av.y * bv.y + av.z * bv.z + av.w * bv.w;
            const float from = 1 - weight, to = weight * (dot < 0 ? -1.0F : 1.0F);
            const float ac[] = {av.x, av.y, av.z, av.w}, bc[] = {bv.x, bv.y, bv.z, bv.w};
            float blended[sizeof ac / sizeof ac[0]];
            for (size_t k = 0; k < sizeof ac / sizeof ac[0]; k++) {
                blended[k] = from * ac[k] + to * bc[k];
            }
            const quatf_t expected = quatf_normalize((quatf_t){blended[0], blended[1], blended[2], blended[3]});
            anim_pose_blend(&a.pose, &b.pose, weights[j], &out.pose);
            TEST_ASSERT_EQUAL_MEMORY(&expected, &out.local[0].rotation, sizeof expected);
        }
    }
    release(&out);
    release(&b);
    release(&a);
}

static void
test_skeleton_binding_failure_index_and_sampling(void) {
    fixture_t f = fixture();
    uint8_t* entry = calloc(1, ENTRY_BYTES);
    TEST_ASSERT_NOT_NULL(entry);
    test_tracks_header(entry, BINDINGS, 1000);
    entry[ANIM_TRACKS_AT_ROOT] = ANIM_ROOT_SKELETON;
    static const char* const FIELDS[] = {"position", "rotation", "scale"};
    static const float SAMPLES[][ANIM_WIDTH_MAX] = {{5, 6, 7, 0}, {0, 0, 0, 1}, {3, 4, 5, 0}};
    for (int c = 0; c < BINDINGS; c++) {
        test_track_row(entry, c,
                       &(test_track_t){"a/child", FIELDS[c], ANIM_COMPONENT_TRANSFORM,
                                       c == ANIM_JOINT_ROTATION ? ANIM_VALUE_QUAT : ANIM_VALUE_VEC3, TIMES,
                                       VALUES + c * ANIM_WIDTH_MAX * sizeof(float), 1, ANIM_STEP});
        test_pack_put_floats(entry + VALUES + c * ANIM_WIDTH_MAX * sizeof(float), SAMPLES[c], ANIM_WIDTH_MAX);
    }
    anim_tracks_t clip;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){entry, ENTRY_BYTES}, &clip));
    anim_joint_bound_t bound[BINDINGS];
    int failed;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind_skeleton(&clip, &SKELETON, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(-1, failed);
    anim_pose_rest(&SKELETON, &f.pose);
    (void)transformf_matrix(&f.local[1]);
    anim_sample_pose(bound, BINDINGS, 0, &f.pose);
    TEST_ASSERT_EQUAL_FLOAT(5, f.local[1].position.x);
    TEST_ASSERT_EQUAL_FLOAT(4, f.local[1].scale.y);
    TEST_ASSERT_EQUAL_FLOAT(1, f.local[1].rotation.w);
    TEST_ASSERT_FALSE(f.local[1].cached);
    TEST_ASSERT_EQUAL_FLOAT(2, f.local[0].position.x);
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_SPACE, anim_bind_skeleton(&clip, &SKELETON, bound, BINDINGS - 1, &failed));
    TEST_ASSERT_EQUAL_INT(-1, failed);
    clip.root = ANIM_ROOT_SCENE;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_ROOT, anim_bind_skeleton(&clip, &SKELETON, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(-1, failed);
    clip.root = ANIM_ROOT_SKELETON;
    uint8_t* row = entry + ANIM_TRACKS_HEADER_SIZE + ANIM_TRACKS_ROW_SIZE;
    test_pack_put32(row + ANIM_TRACKS_ROW_COMPONENT, ASSET_TYPE('B', 'A', 'D', '!'));
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_COMPONENT, anim_bind_skeleton(&clip, &SKELETON, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(1, failed);
    test_pack_put32(row + ANIM_TRACKS_ROW_COMPONENT, ANIM_COMPONENT_TRANSFORM);
    row[ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_VEC3;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_TYPE, anim_bind_skeleton(&clip, &SKELETON, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(1, failed);
    row[ANIM_TRACKS_ROW_TYPE] = ANIM_VALUE_QUAT;
    const int strings = ANIM_TRACKS_HEADER_SIZE + BINDINGS * ANIM_TRACKS_ROW_SIZE;
    entry[strings + TEST_TRACK_STRING_SLOT] = 'z';
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_PATH, anim_bind_skeleton(&clip, &SKELETON, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(1, failed);
    free(entry);
    release(&f);
}

void
suite_anim_pose(void) {
    RUN_TEST(test_rotation_blend_preserves_float_bits);
    RUN_TEST(test_rest_and_multiple_roots);
    RUN_TEST(test_transition_blend_endpoints_midpoint_and_hemisphere);
    RUN_TEST(test_skeleton_binding_failure_index_and_sampling);
}

SUITE_REGISTER(suite_anim_pose);
