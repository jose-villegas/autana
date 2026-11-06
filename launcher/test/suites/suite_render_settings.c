/*
 * Portable suite: render_settings_t, the choices a render context draws
 * with. The default draws as the raster did before it had settings, a
 * setting turned off through the context draws as the development override
 * does, and the override goes on top of the settings live, never into them.
 *
 * The scene is the rig's wall and a card behind the camera, so culling
 * shows in what a frame kept.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "core/tune.h"
#include "render/context/render_context.h"
#include "render/settings/render_settings.h"

#define W           48
#define H           32

/* The rig's card, mirrored through the rig's camera on z: behind it. */
#define CARD_REACH  40
#define CARD_BEHIND (-500)
static const int16_t behind_card[4][3] = {{-CARD_REACH, -CARD_REACH, CARD_BEHIND},
                                          {CARD_REACH, -CARD_REACH, CARD_BEHIND},
                                          {CARD_REACH, CARD_REACH, CARD_BEHIND},
                                          {-CARD_REACH, CARD_REACH, CARD_BEHIND}};
static const int16_t (*const wall_and_behind[RASTER_RIG_QUADS])[3] = {raster_rig_wall, behind_card};

static render_context_t* contexts;
#define CONTEXTS 2

static void
release_all(void) {
    for (int i = 0; contexts != NULL && i < CONTEXTS; i++) {
        render_context_release(&contexts[i]);
    }
    free(contexts);
    contexts = NULL;
    (void)tune_reset(tune_shared(), TUNE_NAME(render, cull));
    raster_rig_release();
}

static raster_rig_t*
open_rig(void) {
    raster_rig_t* r = raster_rig_open(wall_and_behind, RASTER_RIG_QUADS, 0, W, H, 0);
    suite_set_test_cleanup(release_all);
    TEST_ASSERT_TRUE(CARD_BEHIND < raster_rig_camera.eye.z);
    return r;
}

/* CONTEXTS default contexts at full size, too big for the device stack. */
static render_context_t*
open_contexts(void) {
    contexts = malloc(CONTEXTS * sizeof *contexts);
    TEST_ASSERT_NOT_NULL(contexts);
    for (int i = 0; i < CONTEXTS; i++) {
        contexts[i] = RENDER_CONTEXT_DEFAULT;
        render_context_set_scale(&contexts[i], 100);
    }
    return contexts;
}

static int
clusters_of(const raster_rig_t* r, int quad) {
    return r->quad[quad].mesh.cluster_count;
}

static raster_stats_t
draw(render_context_t* c, raster_rig_t* r) {
    const render_view_t view = render_view_fixture_at(&raster_rig_camera, (viewport_t){W, H, 0});
    TEST_ASSERT_TRUE(render_context_draw(c, r->instance, RASTER_RIG_QUADS, &view, 0));
    return c->frame.stats;
}

static void
assert_same_frame(const render_context_t* a, const render_context_t* b) {
    TEST_ASSERT_EQUAL_INT(a->frame.stats.clusters, b->frame.stats.clusters);
    TEST_ASSERT_EQUAL_INT(a->frame.stats.triangles, b->frame.stats.triangles);
    const size_t pixels = (size_t)a->raster.width * (size_t)a->raster.height;
    TEST_ASSERT_EQUAL_MEMORY(raster_color(&a->raster), raster_color(&b->raster), pixels * sizeof(gfx_color_t));
    TEST_ASSERT_EQUAL_MEMORY(raster_depth(&a->raster), raster_depth(&b->raster), pixels * sizeof(uint16_t));
}

static void
test_the_default_is_every_setting_on_and_a_bare_raster_draws_with_it(void) {
    raster_rig_t* r = open_rig();
    const render_settings_t on = {RENDER_SETTINGS_CULL, RENDER_SETTINGS_DEPTH_FIT, RENDER_SETTINGS_BATCHED};
    TEST_ASSERT_TRUE(on.cull && on.depth_fit && on.batched);
    TEST_ASSERT_EQUAL_MEMORY(&on, &RENDER_SETTINGS_DEFAULT, sizeof on);
    const render_settings_t bare = raster_settings(&r->raster);
    TEST_ASSERT_EQUAL_MEMORY(&on, &bare, sizeof on);
    render_context_t* c = open_contexts();
    TEST_ASSERT_EQUAL_MEMORY(&on, render_context_settings(&c[0]), sizeof on);
    /* Culled: the card behind the camera keeps nothing. */
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
    render_context_set_settings(&c[0], &(render_settings_t){.cull = false});
    render_context_release(&c[0]);
    TEST_ASSERT_EQUAL_MEMORY(&on, render_context_settings(&c[0]), sizeof on);
}

static void
test_cull_off_through_the_context_draws_as_the_override_does(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_contexts();
    render_settings_t off = RENDER_SETTINGS_DEFAULT;
    off.cull = false;
    render_context_set_settings(&c[0], &off);
    (void)draw(&c[0], r);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 0));
    (void)draw(&c[1], r);
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0) + clusters_of(r, 1), c[0].frame.stats.clusters);
    assert_same_frame(&c[0], &c[1]);
}

static void
test_the_override_goes_on_top_of_the_settings_live_and_reset_returns_them(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_contexts();
    render_settings_t off = RENDER_SETTINGS_DEFAULT;
    off.cull = false;
    render_context_set_settings(&c[0], &off);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 1));
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
    TEST_ASSERT_FALSE(render_context_settings(&c[0])->cull);
    TEST_ASSERT_TRUE(tune_reset(tune_shared(), TUNE_NAME(render, cull)));
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0) + clusters_of(r, 1), draw(&c[0], r).clusters);
}

void
suite_render_settings(void) {
    RUN_TEST(test_the_default_is_every_setting_on_and_a_bare_raster_draws_with_it);
    RUN_TEST(test_cull_off_through_the_context_draws_as_the_override_does);
    RUN_TEST(test_the_override_goes_on_top_of_the_settings_live_and_reset_returns_them);
}

SUITE_REGISTER(suite_render_settings);
