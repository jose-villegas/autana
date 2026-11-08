/*
 * Portable suite: render_settings_t, the choices a render context draws
 * with: its default, the development override on top of it, and a pack's
 * render entry, read only when it holds every field and no other.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "test_alloc.h"
#include "test_pack.h"
#include "unity.h"

#include "asset/asset_table_format.h"
#include "core/tune.h"
#include "render/context/render_context.h"
#include "render/settings/render_settings.h"
#include "shell/shell_system.h"

#define W           48
#define H           32

/* The rig's card, mirrored through the rig's camera on z: behind it. */
#define CARD_REACH  40
#define CARD_BEHIND (-500)
static const int16_t behind_card[4][3] = {{-CARD_REACH, -CARD_REACH, CARD_BEHIND},
                                          {CARD_REACH, -CARD_REACH, CARD_BEHIND},
                                          {CARD_REACH, CARD_REACH, CARD_BEHIND},
                                          {-CARD_REACH, CARD_REACH, CARD_BEHIND}};
/* The rig's wall and the card behind the camera, so culling shows in what a frame kept. */
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
    (void)tune_reset(tune_shared(), TUNE_NAME(render, lod_coverage_permille));
    (void)tune_reset(tune_shared(), TUNE_NAME(render, lod_tolerance_millipx));
    (void)tune_reset(tune_shared(), TUNE_NAME(render, lod_hysteresis_millipx));
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

/* Field by field: the struct has padding, whose bytes mean nothing. */
static void
assert_settings(const render_settings_t* expected, const render_settings_t* actual) {
    TEST_ASSERT_EQUAL(expected->cull, actual->cull);
    TEST_ASSERT_EQUAL(expected->depth_fit, actual->depth_fit);
    TEST_ASSERT_EQUAL(expected->batched, actual->batched);
    TEST_ASSERT_EQUAL_FLOAT(expected->lod.tolerance_px, actual->lod.tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(expected->lod.hysteresis_px, actual->lod.hysteresis_px);
    TEST_ASSERT_EQUAL_INT(expected->lod.coverage_permille, actual->lod.coverage_permille);
    TEST_ASSERT_EQUAL_INT(expected->lod.pick_every, actual->lod.pick_every);
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
    const render_settings_t on = {
        .cull = true,
        .depth_fit = true,
        .batched = true,
        .lod = {.tolerance_px = 2.0F, .hysteresis_px = 0.5F, .coverage_permille = 250, .pick_every = 4}};
    assert_settings(&on, &RENDER_SETTINGS_DEFAULT);
    const render_settings_t bare = raster_settings(&r->raster);
    assert_settings(&on, &bare);
    render_context_t* c = open_contexts();
    assert_settings(&on, render_context_settings(&c[0]));
    /* Culled: the card behind the camera keeps nothing. */
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
}

static void
test_release_returns_the_settings_to_the_default(void) {
    render_context_t* c = open_contexts();
    const render_settings_t off = cull_off();
    render_context_set_settings(&c[0], &off);
    render_context_release(&c[0]);
    assert_settings(&RENDER_SETTINGS_DEFAULT, render_context_settings(&c[0]));
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

/* The override never writes the caller's value, so a reset brings that value
 * back, not the default. */
static void
test_an_override_beats_the_callers_value_until_a_reset_brings_it_back(void) {
    raster_rig_t* r = open_rig();
    render_context_t* c = open_contexts();
    const render_settings_t off = cull_off();
    render_context_set_settings(&c[0], &off);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, cull), 1));
    TEST_ASSERT_EQUAL_INT(clusters_of(r, 0), draw(&c[0], r).clusters);
    assert_settings(&off, render_context_settings(&c[0]));
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

/* The coverage cap's knob changes that field of what a draw uses and no other, and a reset gives the caller's
 * value back. */
static void
test_the_coverage_knob_overrides_only_the_coverage_cap(void) {
    raster_rig_t* r = open_rig();
    render_settings_t set = RENDER_SETTINGS_DEFAULT;
    set.lod = (render_lod_settings_t){3.0F, 0.25F, 100, 2};
    r->raster.settings = &set;
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, lod_coverage_permille), 500));
    render_settings_t used = raster_settings(&r->raster);
    TEST_ASSERT_EQUAL_INT(500, used.lod.coverage_permille);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, used.lod.tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, used.lod.hysteresis_px);
    TEST_ASSERT_EQUAL_INT(2, used.lod.pick_every);
    TEST_ASSERT_EQUAL_INT(100, set.lod.coverage_permille);
    TEST_ASSERT_TRUE(tune_reset(tune_shared(), TUNE_NAME(render, lod_coverage_permille)));
    used = raster_settings(&r->raster);
    TEST_ASSERT_EQUAL_INT(100, used.lod.coverage_permille);
}

/* The tolerance and hysteresis knobs, in thousandths of a pixel, change those fields of what a draw uses and no
 * other, and a reset gives the caller's values back. */
static void
test_the_tolerance_knobs_override_the_pick_in_thousandths_of_a_pixel(void) {
    raster_rig_t* r = open_rig();
    render_settings_t set = RENDER_SETTINGS_DEFAULT;
    set.lod = (render_lod_settings_t){3.0F, 0.25F, 100, 2};
    r->raster.settings = &set;
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, lod_tolerance_millipx), 1500));
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render, lod_hysteresis_millipx), 0));
    render_settings_t used = raster_settings(&r->raster);
    TEST_ASSERT_EQUAL_FLOAT(1.5F, used.lod.tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(0.0F, used.lod.hysteresis_px);
    TEST_ASSERT_EQUAL_INT(100, used.lod.coverage_permille);
    TEST_ASSERT_EQUAL_INT(2, used.lod.pick_every);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, set.lod.tolerance_px);
    TEST_ASSERT_TRUE(tune_reset(tune_shared(), TUNE_NAME(render, lod_tolerance_millipx)));
    TEST_ASSERT_TRUE(tune_reset(tune_shared(), TUNE_NAME(render, lod_hysteresis_millipx)));
    used = raster_settings(&r->raster);
    TEST_ASSERT_EQUAL_FLOAT(3.0F, used.lod.tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, used.lod.hysteresis_px);
}

enum { FIELDS = RENDER_SETTINGS_FIELD_COUNT, ROW_BYTES = ASSET_TABLE_NAME_SIZE + sizeof(int16_t), PACK_BYTES = 512 };

static const char* const field_names[FIELDS + 1] = {
    "cull",           "depth_fit", "batched", "lod.tolerance_px", "lod.hysteresis_px", "lod.coverage_permille",
    "lod.pick_every", "fog"};
#define STEP (1 << RENDER_SETTINGS_FRAC_BITS)

/* Opens into `opened` a pack whose render entry has the first `rows` of
 * field_names, row i holding `values[i]`, with `frac` fractional bits; free
 * `*raw` with test_free_aligned(). */
static void
open_entry(int rows, const int16_t* values, int frac, asset_pack_t* opened, void** raw) {
    uint8_t* bytes = test_alloc_aligned(PACK_BYTES, ASSET_PACK_BASE_ALIGN, raw);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, PACK_BYTES, 1);
    uint8_t* entry = test_pack_add(&pack, RENDER_SETTINGS_ENTRY, ASSET_TABLE_ASSET,
                                   ASSET_TABLE_HEADER_SIZE + ((uint32_t)rows * ROW_BYTES));
    test_pack_put16(entry, ASSET_TABLE_VERSION);
    test_pack_put16(entry + ASSET_TABLE_AT_ROWS, rows);
    test_pack_put16(entry + ASSET_TABLE_AT_COLUMNS, 1);
    entry[ASSET_TABLE_AT_FRAC_BITS] = (uint8_t)frac;
    for (int i = 0; i < rows; i++) {
        uint8_t* row = entry + ASSET_TABLE_HEADER_SIZE + (i * ROW_BYTES);
        memcpy(row, field_names[i], strlen(field_names[i]));
        test_pack_put16(row + ASSET_TABLE_NAME_SIZE, values[i]);
    }
    TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(opened, bytes, test_pack_finish(&pack)));
}

/* render_settings_read() of open_entry()'s pack; `out` starts all on. */
static asset_status_t
read_entry(int rows, const int16_t* values, int frac, render_settings_t* out) {
    void* raw = NULL;
    asset_pack_t opened;
    open_entry(rows, values, frac, &opened, &raw);
    *out = RENDER_SETTINGS_DEFAULT;
    const asset_status_t status = render_settings_read(&opened, out);
    test_free_aligned(raw);
    return status;
}

static void
test_a_packs_render_entry_reads_every_field(void) {
    static const int16_t values[FIELDS] = {0, STEP, 0, (3 * STEP) + (STEP / 2), STEP / 4, 100 * STEP, 2 * STEP};
    render_settings_t read;
    TEST_ASSERT_EQUAL(ASSET_OK, read_entry(FIELDS, values, RENDER_SETTINGS_FRAC_BITS, &read));
    TEST_ASSERT_FALSE(read.cull);
    TEST_ASSERT_TRUE(read.depth_fit);
    TEST_ASSERT_FALSE(read.batched);
    TEST_ASSERT_EQUAL_FLOAT(3.5F, read.lod.tolerance_px);
    TEST_ASSERT_EQUAL_FLOAT(0.25F, read.lod.hysteresis_px);
    TEST_ASSERT_EQUAL_INT(100, read.lod.coverage_permille);
    TEST_ASSERT_EQUAL_INT(2, read.lod.pick_every);
}

static void
test_an_entry_missing_a_field_with_another_or_fixed_point_is_refused_unread(void) {
    static const int16_t values[FIELDS + 1] = {0};
    render_settings_t read;
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, read_entry(FIELDS - 1, values, RENDER_SETTINGS_FRAC_BITS, &read));
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, read_entry(FIELDS + 1, values, RENDER_SETTINGS_FRAC_BITS, &read));
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, read_entry(FIELDS, values, 0, &read));
    assert_settings(&RENDER_SETTINGS_DEFAULT, &read);
}

static void
test_a_pack_without_the_entry_reads_not_found(void) {
    void* raw = NULL;
    uint8_t* bytes = test_alloc_aligned(PACK_BYTES, ASSET_PACK_BASE_ALIGN, &raw);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, PACK_BYTES, 1);
    (void)test_pack_add(&pack, "ridge", ASSET_TABLE_ASSET, ASSET_TABLE_HEADER_SIZE);
    asset_pack_t opened;
    render_settings_t read = RENDER_SETTINGS_DEFAULT;
    asset_status_t status = asset_pack_open(&opened, bytes, test_pack_finish(&pack));
    if (status == ASSET_OK) {
        status = render_settings_read(&opened, &read);
    }
    test_free_aligned(raw);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND, status);
}

/* The shell's render system, as registered. */
static const shell_system_t*
render_system(void) {
    shell_system_t* const real = shell_system_swap_for_test(NULL);
    shell_system_swap_for_test(real);
    const shell_system_t* system = real;
    while (system != NULL && strcmp(system->name, "render") != 0) {
        system = system->next;
    }
    TEST_ASSERT_NOT_NULL(system);
    TEST_ASSERT_NOT_NULL(system->app_enter);
    return system;
}

static void
test_an_app_starts_with_its_packs_settings_and_without_one_the_default(void) {
    static const int16_t values[FIELDS] = {0};
    const render_settings_t off = {0};
    void* raw = NULL;
    asset_pack_t opened;
    open_entry(FIELDS, values, RENDER_SETTINGS_FRAC_BITS, &opened, &raw);
    render_system()->app_enter(&opened);
    test_free_aligned(raw);
    assert_settings(&off, render_context_settings(render_context_main()));
    render_system()->app_enter(NULL);
    assert_settings(&RENDER_SETTINGS_DEFAULT, render_context_settings(render_context_main()));
}

void
suite_render_settings(void) {
    RUN_TEST(test_the_default_is_every_setting_on_as_the_raster_drew_before);
    RUN_TEST(test_release_returns_the_settings_to_the_default);
    RUN_TEST(test_the_call_and_the_knob_each_take_effect_the_next_frame);
    RUN_TEST(test_an_override_beats_the_callers_value_until_a_reset_brings_it_back);
    RUN_TEST(test_each_field_and_each_knob_reach_only_their_own_slot);
    RUN_TEST(test_the_coverage_knob_overrides_only_the_coverage_cap);
    RUN_TEST(test_the_tolerance_knobs_override_the_pick_in_thousandths_of_a_pixel);
    RUN_TEST(test_a_release_build_carries_no_override);
    RUN_TEST(test_a_packs_render_entry_reads_every_field);
    RUN_TEST(test_an_entry_missing_a_field_with_another_or_fixed_point_is_refused_unread);
    RUN_TEST(test_a_pack_without_the_entry_reads_not_found);
    RUN_TEST(test_an_app_starts_with_its_packs_settings_and_without_one_the_default);
}

SUITE_REGISTER(suite_render_settings);
