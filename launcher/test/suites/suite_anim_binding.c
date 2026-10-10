/* Portable binding resolution and writes into component structs. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_binding.h"
#include "render/r3d_scene.h"
#include "suites.h"
#include "test_anim_tracks.h"
#include "unity.h"

enum { BINDINGS = 3, BYTES = 512, TIMES = 300, VALUES = 304, DIRTY_BIT = 4, OTHER_DIRTY_BIT = 1 };

typedef struct {
    float position[3], rotation[4], scale[3];
} transform_t;

static const anim_field_t TRANSFORM_FIELDS[] = {
    {"position", ANIM_VALUE_VEC3, offsetof(transform_t, position)},
    {"rotation", ANIM_VALUE_QUAT, offsetof(transform_t, rotation)},
    {"scale", ANIM_VALUE_VEC3, offsetof(transform_t, scale)},
};
static const anim_component_fields_t TRANSFORM = {
    ANIM_COMPONENT_TRANSFORM,
    TRANSFORM_FIELDS,
    sizeof TRANSFORM_FIELDS / sizeof TRANSFORM_FIELDS[0],
};

typedef struct {
    uint8_t* entry;
    anim_tracks_t clip;
} fixture_t;

static fixture_t
fixture(void) {
    fixture_t f = {.entry = calloc(1, BYTES)};
    TEST_ASSERT_NOT_NULL(f.entry);
    test_tracks_header(f.entry, BINDINGS, 1000);
    static const char* const NAMES[] = {"position", "rotation", "scale"};
    for (int i = 0; i < BINDINGS; i++) {
        test_track_row(f.entry, i,
                       &(test_track_t){.path = "object",
                                       .field = NAMES[i],
                                       .component = ANIM_COMPONENT_TRANSFORM,
                                       .times = TIMES,
                                       .values = VALUES + i * ANIM_WIDTH_MAX * sizeof(float),
                                       .keys = 1,
                                       .type = i == 1 ? ANIM_VALUE_QUAT : ANIM_VALUE_VEC3,
                                       .interp = ANIM_STEP});
        const float values[] = {2, 4, 6, 8};
        test_pack_put_floats(f.entry + VALUES + i * ANIM_WIDTH_MAX * sizeof(float), values, ANIM_WIDTH_MAX);
    }
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){f.entry, BYTES}, &f.clip));
    return f;
}

static void
test_resolution_errors_report_first_index(void) {
    fixture_t f = fixture();
    transform_t transform = {0};
    anim_component_ref_t components[] = {{&TRANSFORM, &transform}};
    anim_target_t target = {.name = "object", .components = components, .component_count = 1};
    anim_bound_t bound[BINDINGS];
    int failed;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(-1, failed);
    anim_binding_t binding;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(&f.clip, 0, &binding));
    anim_bound_t field;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind_field(&binding, components, 1, NULL, 0, &field));
    TEST_ASSERT_EQUAL_PTR(transform.position, field.target);
    binding.component = ASSET_TYPE('T', 'E', 'S', 'T');
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_COMPONENT, anim_bind_field(&binding, components, 1, NULL, 0, &field));
    f.clip.root = ANIM_ROOT_SKELETON;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_ROOT, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(-1, failed);
    f.clip.root = ANIM_ROOT_SCENE;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_SPACE, anim_bind(&f.clip, &target, 1, bound, BINDINGS - 1, &failed));
    TEST_ASSERT_EQUAL_INT(-1, failed);
    target.name = "other";
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_PATH, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    target.name = "object";
    target.component_count = 0;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_COMPONENT, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    target.component_count = 1;
    anim_component_fields_t incomplete = TRANSFORM;
    incomplete.field_count = 1;
    components[0].fields = &incomplete;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_FIELD, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(1, failed);
    anim_field_t fields[BINDINGS];
    memcpy(fields, TRANSFORM_FIELDS, sizeof fields);
    fields[1].type = ANIM_VALUE_VEC3;
    incomplete.fields = fields;
    incomplete.field_count = BINDINGS;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_TYPE, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    TEST_ASSERT_EQUAL_INT(1, failed);
    free(f.entry);
}

static void
test_apply_split_writes_sampled_values_and_marks_dirty(void) {
    fixture_t f = fixture();
    transform_t full = {0}, split = {0};
    uint8_t dirty = OTHER_DIRTY_BIT;
    anim_component_ref_t components[] = {{&TRANSFORM, &full}};
    anim_target_t target = {
        .name = "object", .components = components, .component_count = 1, .dirty = &dirty, .dirty_bit = DIRTY_BIT};
    anim_bound_t bound[BINDINGS];
    int failed;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    float sampled[ANIM_WIDTH_MAX];
    anim_track_sample(&bound[1].curve, 0, sampled);
    anim_apply(bound, BINDINGS, 0);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(sampled, full.rotation, ANIM_WIDTH_MAX);
    TEST_ASSERT_EQUAL_UINT8(OTHER_DIRTY_BIT | DIRTY_BIT, dirty);
    float norm = 0;
    for (int i = 0; i < ANIM_WIDTH_MAX; i++) {
        norm += sampled[i] * sampled[i];
    }
    TEST_ASSERT_FLOAT_WITHIN(0.00001F, 1.0F, norm);
    components[0].base = &split;
    target.dirty = NULL;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind(&f.clip, &target, 1, bound, BINDINGS, NULL));
    anim_apply(bound, 1, 0);
    anim_apply(bound + 1, BINDINGS - 1, 0);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY((float*)&full, (float*)&split, sizeof full / sizeof(float));
    free(f.entry);
}

static void
test_binding_description_has_component_order_and_snprintf_length(void) {
    const anim_binding_t b = {.path = "object", .component = ANIM_COMPONENT_TRANSFORM, .field = "position"};
    const char* want = "object:TRNS.position";
    char text[64];
    TEST_ASSERT_EQUAL_INT(strlen(want), anim_binding_describe(&b, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING(want, text);
    TEST_ASSERT_EQUAL_INT(strlen(want), anim_binding_describe(&b, text, 1));
    TEST_ASSERT_EQUAL_STRING("", text);
    TEST_ASSERT_EQUAL_INT(strlen(want), anim_binding_describe(&b, NULL, 0));
    TEST_ASSERT_EQUAL_INT(ANIM_COMPONENT_CAMERA, R3D_SCENE_CAMERA_FIELDS.component);
    TEST_ASSERT_EQUAL_STRING("half_fov_short_tan", R3D_SCENE_CAMERA_FIELDS.fields[0].name);
}

void
suite_anim_binding(void) {
    RUN_TEST(test_resolution_errors_report_first_index);
    RUN_TEST(test_apply_split_writes_sampled_values_and_marks_dirty);
    RUN_TEST(test_binding_description_has_component_order_and_snprintf_length);
}

SUITE_REGISTER(suite_anim_binding);
