/*
 * suite_baked_families: every family render_lab shows by its bake has, in its
 * pack, that renderer and the camera it starts on, and its loaded scene
 * leaves the app heap the room a profiled visit takes.
 */
#include "suites.h"
#include "unity.h"

#include "app/app_arena.h"
#include "apps/render_lab/baked_families.h"
#include "apps/render_lab/sponza_content.h"
#include "core/memory.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"

static void
test_every_family_places_its_bake_and_its_camera(void) {
    for (int f = 0; f < BAKED_FAMILY_COUNT; f++) {
        const scene_baked_t* family = baked_families[f];
        const size_t mark = app_arena_mark();
        entity_t scene = scene_load(family->scene);
        TEST_ASSERT_FALSE_MESSAGE(handle_is_none(scene), family->scene);
        TEST_ASSERT_EQUAL_INT_MESSAGE(PENDING_READY, scene_state(scene), family->scene);
        const entity_t bake = entity_find(scene, name_hash(family->bake));
        TEST_ASSERT_FALSE_MESSAGE(handle_is_none(bake), family->bake);
        TEST_ASSERT_NOT_NULL_MESSAGE(renderer_mesh(bake), family->bake);
        TEST_ASSERT_FALSE_MESSAGE(handle_is_none(entity_find(scene, name_hash(family->camera))), family->camera);
        scene_release(scene);
        app_arena_rewind(mark);
    }
}

/* The block a loaded family must leave free in the app heap: room for what
 * a visit takes beside the scene at its fullest, the frame profile of the
 * longest camera path (suite_render_lab_frame_profile.c): its frame scratch,
 * its two trace buffers and the cull view's state. One block, since the
 * scratch is one take. */
#define FAMILY_VISIT_BYTES (2560U * 1024U)

/* The bake keeps its meshes' copies until the scene closes, so a bake that
 * grows any of them takes from a visit: this names the family and the bytes
 * short, where the visit itself would only fail to draw. */
static void
test_every_family_leaves_a_profiled_visit_its_room(void) {
    for (int f = 0; f < BAKED_FAMILY_COUNT; f++) {
        const scene_baked_t* family = baked_families[f];
        const size_t mark = app_arena_mark();
        entity_t scene = scene_load(family->scene);
        TEST_ASSERT_EQUAL_INT_MESSAGE(PENDING_READY, scene_state(scene), family->scene);
        (void)scene_bounds(scene); /* resolves the scene: the bake takes its copies */
        scene_failure_t why;
        TEST_ASSERT_FALSE_MESSAGE(scene_failure(scene, &why), family->scene);
        const size_t left = memory_largest_block(MEMORY_APP);
        scene_release(scene);
        app_arena_rewind(mark);
        TEST_ASSERT_GREATER_OR_EQUAL_size_t_MESSAGE(FAMILY_VISIT_BYTES, left, family->scene);
    }
}

static void
run_baked_families_suite(void) {
    RUN_TEST(test_every_family_places_its_bake_and_its_camera);
    RUN_TEST(test_every_family_leaves_a_profiled_visit_its_room);
}

SUITE_REGISTER(run_baked_families_suite);
SUITE_READS(run_baked_families_suite, SPONZA_SCENE, BISTRO_SCENE);
