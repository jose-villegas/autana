/*
 * Portable suite: render_settings_t, the choices a render context draws
 * with. The default draws as the raster did before it had settings; a
 * development override (TUNE_OVERRIDE) goes on top of the caller's value
 * without writing it, and its reset brings that value back.
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
release_contexts(void) {
    for (int i = 0; contexts != NULL && i < CONTEXTS; i++) {
        render_context_release(&contexts[i]);
    }
    free(contexts);
    contexts = NULL;
}

static void
release_all(void) {
    release_contexts();
    (void)tune_reset(tune_shared(), TUNE_NAME(render, cull));
    (void)tune_reset(tune_shared(), TUNE_NAME(render, depth_fit));
    (void)tune_reset(tune_shared(), TUNE_NAME(render, batched));
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
    suite_set_test_cleanup(release_all);
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

/* What TUNE_OVERRIDE(owner, what, 1) is in a release build. */
TUNE_CONSTANT(released, knob, TUNE_UNSET, TUNE_UNSET, 1);

static render_settings_t
cull_off(void) {
    render_settings_t off = RENDER_SETTINGS_DEFAULT;
    off.cull = false;
    return off;
}

static void
test_the_default_is_every_setting_on_as_the_raster_drew_before(void) {
    raster_rig_t* r = open_rig();
    /* Before settings, the raster always culled, fitted depth and batched. */
    const render_settings_t on = {.cull = true, .depth_fit = true, .batched = true};
    TEST_ASSERT_EQUAL_MEMORY(&on, &RENDER_SETTINGS_DEFAULT, sizeof on);
    const render_settings_t bare = raster_settings(&r->raster);
    TEST_ASSERT_EQUAL_MEMORY(&on, &bare, sizeof on);
    render_context_t* c = open_contexts();
    TEST_ASSERT_EQUAL_MEMORY(&on, render_context_settings(&c[0]), sizeof on);
    /* Culled: the card behind the camera keeps nothing. */
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
}

static void
test_release_returns_the_settings_to_the_default(void) {
    render_context_t* c = open_contexts();
    const render_settings_t off = cull_off();
    render_context_set_settings(&c[0], &off);
    render_context_release(&c[0]);
    TEST_ASSERT_EQUAL_MEMORY(&RENDER_SETTINGS_DEFAULT, render_context_settings(&c[0]), sizeof(render_settings_t));
}

static void
test_the_call_and_the_knob_each_take_effect_the_next_frame(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_contexts();
    const int kept = clusters_of(r, 0) + clusters_of(r, 1);
    for (int i = 0; i < CONTEXTS; i++) {
        TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[i], r).clusters);
    }
    const render_settings_t off = cull_off();
    render_context_set_settings(&c[0], &off);
    TEST_ASSERT_EQUAL_INT(kept, draw(&c[0], r).clusters);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 0));
    TEST_ASSERT_EQUAL_INT(kept, draw(&c[1], r).clusters);
    assert_same_frame(&c[0], &c[1]);
}

static void
test_an_override_beats_the_callers_value_and_leaves_it_as_set(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_contexts();
    const render_settings_t off = cull_off();
    render_context_set_settings(&c[0], &off);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 1));
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
    TEST_ASSERT_EQUAL_MEMORY(&off, render_context_settings(&c[0]), sizeof off);
}

static void
test_a_reset_brings_back_the_callers_value_not_the_default(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_contexts();
    const render_settings_t off = cull_off();
    render_context_set_settings(&c[0], &off);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 1));
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
    TEST_ASSERT_TRUE(tune_reset(tune_shared(), TUNE_NAME(render, cull)));
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0) + clusters_of(r, 1), draw(&c[0], r).clusters);
}

/* Whether every field of `s` is on but field `off`, which is off; SIZE_MAX for none. */
static void
assert_only_off(render_settings_t s, size_t off) {
    const bool fields[] = {s.cull, s.depth_fit, s.batched};
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
        TEST_ASSERT_EQUAL(f != off, fields[f]);
    }
}

static void
test_each_field_and_each_knob_reach_only_their_own_slot(void) {
    raster_rig_t* r = open_rig();
    static const char* const knobs[] = {TUNE_NAME(render, cull), TUNE_NAME(render, depth_fit),
                                        TUNE_NAME(render, batched)};
    render_settings_t set;
    bool* const fields[] = {&set.cull, &set.depth_fit, &set.batched};
    _Static_assert(sizeof fields / sizeof fields[0] == sizeof knobs / sizeof knobs[0], "a field per knob");
    r->raster.settings = &set;
    for (size_t k = 0; k < sizeof knobs / sizeof knobs[0]; k++) {
        set = RENDER_SETTINGS_DEFAULT;
        *fields[k] = false;
        assert_only_off(raster_settings(&r->raster), k);
        TEST_ASSERT_TRUE(tune_set(tune_shared(), knobs[k], 1));
        assert_only_off(raster_settings(&r->raster), SIZE_MAX);
        set = RENDER_SETTINGS_DEFAULT;
        TEST_ASSERT_TRUE(tune_set(tune_shared(), knobs[k], 0));
        assert_only_off(raster_settings(&r->raster), k);
        TEST_ASSERT_TRUE(tune_reset(tune_shared(), knobs[k]));
    }
}

static void
test_a_release_build_carries_no_override(void) {
    TEST_ASSERT_EQUAL_INT(TUNE_UNSET, knob);
    TEST_ASSERT_EQUAL_INT(0, tune_override(knob, 0));
    TEST_ASSERT_EQUAL_INT(1, tune_override(knob, 1));
}

void
suite_render_settings(void) {
    RUN_TEST(test_the_default_is_every_setting_on_as_the_raster_drew_before);
    RUN_TEST(test_release_returns_the_settings_to_the_default);
    RUN_TEST(test_the_call_and_the_knob_each_take_effect_the_next_frame);
    RUN_TEST(test_an_override_beats_the_callers_value_and_leaves_it_as_set);
    RUN_TEST(test_a_reset_brings_back_the_callers_value_not_the_default);
    RUN_TEST(test_each_field_and_each_knob_reach_only_their_own_slot);
    RUN_TEST(test_a_release_build_carries_no_override);
}

SUITE_REGISTER(suite_render_settings);
