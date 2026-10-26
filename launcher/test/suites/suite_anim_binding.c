/* Portable binding resolution and writes into component structs. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "anim/anim_binding.h"
#include "anim/anim_tracks_format.h"
#include "scene/camera/camera.h"
#include "suites.h"
#include "test_anim_tracks.h"
#include "unity.h"

enum { BINDINGS = 3, BYTES = 512, TIMES = 300, VALUES = 304, DIRTY_BIT = 4, OTHER_DIRTY_BIT = 1 };

typedef struct {
    float position[3], rotation[4], scale[3];
} transform_t;

static const anim_field_t TRANSFORM_FIELDS[] = {
    ANIM_FIELD(transform_t, position, ANIM_VALUE_VEC3),
    ANIM_FIELD(transform_t, rotation, ANIM_VALUE_QUAT),
    ANIM_FIELD(transform_t, scale, ANIM_VALUE_VEC3),
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
    anim_target_t target = {.name = name_hash("object"), .components = components, .component_count = 1};
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
    target.name = name_hash("other");
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_PATH, anim_bind(&f.clip, &target, 1, bound, BINDINGS, &failed));
    target.name = name_hash("object");
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
    anim_target_t target = {.name = name_hash("object"),
                            .components = components,
                            .component_count = 1,
                            .dirty = &dirty,
                            .dirty_bit = DIRTY_BIT};
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
test_field_binding_distinguishes_hash_collisions(void) {
    fixture_t f = fixture();
    transform_t transform = {0};
    const char* first = "costarring";
    const char* second = "liquid";
    TEST_ASSERT_EQUAL_UINT32(name_hash(first), name_hash(second));
    const anim_field_t fields[] = {{first, ANIM_VALUE_VEC3, offsetof(transform_t, position)},
                                   {second, ANIM_VALUE_VEC3, offsetof(transform_t, scale)}};
    const anim_component_fields_t component = {ANIM_COMPONENT_TRANSFORM, fields, 2};
    const anim_component_ref_t reference = {&component, &transform};
    anim_binding_t binding;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_binding_at(&f.clip, 0, &binding));
    binding.field = second;
    binding.field_hash = name_hash(second);
    anim_bound_t bound;
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind_field(&binding, &reference, 1, NULL, 0, &bound));
    TEST_ASSERT_EQUAL_PTR(transform.scale, bound.target);
    binding.field = "missing";
    binding.field_hash = name_hash(binding.field);
    TEST_ASSERT_EQUAL_INT(ANIM_BIND_ERR_FIELD, anim_bind_field(&binding, &reference, 1, NULL, 0, &bound));
    free(f.entry);
}

static void
test_self_sentinel_distinguishes_hash_collisions(void) {
    fixture_t f = fixture();
    transform_t self = {0}, other = {0};
    const anim_component_ref_t components[] = {{&TRANSFORM, &self}, {&TRANSFORM, &other}};
    const char* collision = "mfvbkp";
    TEST_ASSERT_EQUAL_UINT32(name_hash(ANIM_TARGET_SELF), name_hash(collision));
    const anim_target_t targets[] = {
        {.name = name_hash("owner"), .components = &components[0], .component_count = 1},
        {.name = name_hash(collision), .components = &components[1], .component_count = 1},
    };
    const char* paths[] = {ANIM_TARGET_SELF, collision};
    for (size_t p = 0; p < sizeof paths / sizeof paths[0]; p++) {
        for (int i = 0; i < BINDINGS; i++) {
            test_track_row(f.entry, i,
                           &(test_track_t){paths[p], TRANSFORM_FIELDS[i].name, ANIM_COMPONENT_TRANSFORM,
                                           TRANSFORM_FIELDS[i].type, TIMES, VALUES + i * ANIM_WIDTH_MAX * sizeof(float),
                                           1, ANIM_STEP});
        }
        TEST_ASSERT_EQUAL_INT(ASSET_OK, anim_tracks_open((asset_view_t){f.entry, BYTES}, &f.clip));
        anim_bound_t bound[BINDINGS];
        TEST_ASSERT_EQUAL_INT(ANIM_BIND_OK, anim_bind(&f.clip, targets, 2, bound, BINDINGS, NULL));
        TEST_ASSERT_EQUAL_PTR(p == 0 ? self.position : other.position, bound[0].target);
    }
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
    TEST_ASSERT_EQUAL_INT(ANIM_COMPONENT_CAMERA, CAMERA_FIELDS.component);
    TEST_ASSERT_EQUAL_STRING("half_fov_short_tan", CAMERA_FIELDS.fields[0].name);
}

void
suite_anim_binding(void) {
    RUN_TEST(test_field_binding_distinguishes_hash_collisions);
    RUN_TEST(test_self_sentinel_distinguishes_hash_collisions);
    RUN_TEST(test_resolution_errors_report_first_index);
    RUN_TEST(test_apply_split_writes_sampled_values_and_marks_dirty);
    RUN_TEST(test_binding_description_has_component_order_and_snprintf_length);
}

SUITE_REGISTER(suite_anim_binding);
