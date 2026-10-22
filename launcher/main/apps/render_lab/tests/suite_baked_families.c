/*
 * suite_baked_families: every family render_lab shows one bake at a time has,
 * in its pack, a renderer for each variant and the camera it starts on.
 */
#include "suites.h"
#include "unity.h"

#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/sponza_content.h"
#include "scene/scene.h"

static void
test_every_family_places_each_variant_and_its_camera(void) {
    for (int f = 0; f < BAKED_FAMILY_COUNT; f++) {
        const scene_baked_t* family = baked_families[f];
        scene_failure_t why;
        scene_t* scene = scene_load(family->scene, &why);
        TEST_ASSERT_NOT_NULL_MESSAGE(scene, family->scene);
        for (int v = 0; v < (int)SCENE_BAKED_VARIANT_COUNT; v++) {
            const scene_entity_t bake = scene_find(scene, family->bakes[v]);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(SCENE_ENTITY_NONE, bake, family->bakes[v]);
            TEST_ASSERT_NOT_NULL_MESSAGE(scene_entity_mesh(scene, bake), family->bakes[v]);
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE(SCENE_ENTITY_NONE, scene_find(scene, family->camera), family->camera);
        scene_unload(scene);
    }
}

static void
run_baked_families_suite(void) {
    RUN_TEST(test_every_family_places_each_variant_and_its_camera);
}

SUITE_REGISTER(run_baked_families_suite);
SUITE_READS(run_baked_families_suite, SPONZA_SCENE, BISTRO_SCENE);
