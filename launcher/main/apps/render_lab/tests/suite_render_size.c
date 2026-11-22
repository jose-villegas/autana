#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/ui/render_lab_settings.h"
#include "lab/ui/lab_menu_screen.h"

#include "core/tune.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui_probe.h"
#include "unity.h"

static void
fixture(void) {
    static const char* const tuned[] = {TUNE_NAME(render_lab, scale),
                                        TUNE_NAME(render_lab, present),
                                        TUNE_NAME(render_lab, small_fill_ceiling),
                                        TUNE_NAME(render_lab, small_fill_gate),
                                        TUNE_NAME(render_lab, small_fill_style),
                                        TUNE_NAME(render_lab, budget),
                                        TUNE_NAME(render_lab, framegen),
                                        TUNE_NAME(render_lab, upscale)};
    for (size_t i = 0; i < sizeof tuned / sizeof tuned[0]; i++) {
        TEST_ASSERT_TRUE(tune_reset(tune_shared(), tuned[i]));
    }
}

static void
test_render_size_and_percent_follow_and_clamp(void) {
    fixture();
    resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(184, size.width);
    TEST_ASSERT_EQUAL_INT(224, size.height);
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_NEAREST, lab_view_present(&render_lab_view_tunables));
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, present), 1));
    size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(184, size.width);
    TEST_ASSERT_EQUAL_INT(112, size.height);
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_SCANLINE, lab_view_present(&render_lab_view_tunables));
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, present), 2));
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_DOUBLE, lab_view_present(&render_lab_view_tunables));
    TEST_ASSERT_FALSE(tune_set(tune_shared(), TUNE_NAME(render_lab, present), 3));
    fixture();
}

static void
test_small_fill_ceiling_range(void) {
    fixture();
    tune_registry_t* registry = tune_shared();
    const char* name = TUNE_NAME(render_lab, small_fill_ceiling);
    TEST_ASSERT_TRUE(tune_set(registry, name, SMALL_FILL_CEILING_MAX));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_MAX, render_lab_small_fill().ceiling);
    TEST_ASSERT_FALSE(tune_set(registry, name, SMALL_FILL_CEILING_MAX + 1));
    TEST_ASSERT_FALSE(tune_set(registry, name, UINT8_MAX));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_MAX, render_lab_small_fill().ceiling);
    TEST_ASSERT_TRUE(tune_set(registry, name, 0));
    TEST_ASSERT_EQUAL_INT(0, render_lab_small_fill().ceiling);
    TEST_ASSERT_FALSE(tune_set(registry, name, -1));
    TEST_ASSERT_TRUE(tune_reset(registry, name));
}

static void
test_small_fill_menu_choice_moves_the_tunables(void) {
    fixture();
    int count = 0;
    const char* const* names = render_lab_small_fill_names(&count);
    TEST_ASSERT_EQUAL_STRING("SMOOTH", names[0]);
    TEST_ASSERT_EQUAL_STRING("FAST", names[1]);
    TEST_ASSERT_EQUAL_STRING("FAST - DITHER", names[2]);
    TEST_ASSERT_EQUAL_STRING("FAST - GRAIN", names[3]);
    TEST_ASSERT_EQUAL_INT(4, count);
    TEST_ASSERT_EQUAL_INT(0, render_lab_small_fill_selected());

    TEST_ASSERT_TRUE(render_lab_small_fill_select(1));
    TEST_ASSERT_EQUAL_INT(1, render_lab_small_fill_selected());
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_FAST, render_lab_small_fill().ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_GATE_FAST, render_lab_small_fill().gate);

    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, small_fill_gate), 3));
    TEST_ASSERT_EQUAL_INT(3, render_lab_small_fill().gate);
    TEST_ASSERT_FALSE(render_lab_small_fill_select(count));
    TEST_ASSERT_EQUAL_INT(1, render_lab_small_fill_selected());

    TEST_ASSERT_TRUE(render_lab_small_fill_select(2));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_FAST_DITHER.ceiling, render_lab_small_fill().ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_FAST_DITHER.gate, render_lab_small_fill().gate);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_DITHER, render_lab_small_fill().style);
    TEST_ASSERT_TRUE(render_lab_small_fill_select(3));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_GRAIN, render_lab_small_fill().style);

    TEST_ASSERT_TRUE(render_lab_small_fill_select(0));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_SMOOTH, render_lab_small_fill().ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_FLAT, render_lab_small_fill().style);
    fixture();
}

/* The toggle is the budget: on sets RENDER_LAB_FRAME_TARGET_MS, any budget a
 * console sets reads as on, and apply hands it to the context with the
 * render_lab ladder, for whichever scene is showing. */
static void
test_dynamic_resolution_toggle_moves_the_budget_into_the_context(void) {
    fixture();
    render_lab_dynres_stop();
    const render_context_t* context = render_context_main();
    TEST_ASSERT_FALSE(render_lab_dynres_on());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_FIXED, context->policy);

    TEST_ASSERT_TRUE(render_lab_dynres_set_on(true));
    TEST_ASSERT_TRUE(render_lab_dynres_on());
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_FRAME_TARGET_MS, render_lab_budget_ms());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_PREDICTED, context->policy);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_FRAME_TARGET_MS * MATH_US_PER_MS, context->ladder.budget_us);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_LADDER_STEPS, context->ladder.step_count);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_LADDER_HALF, context->predict.step);

    /* A scene switch stops it; the next scene's apply hands it again. */
    render_lab_dynres_stop();
    TEST_ASSERT_EQUAL_INT(RENDER_FIXED, context->policy);
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_PREDICTED, context->policy);

    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, budget), 75));
    TEST_ASSERT_TRUE(render_lab_dynres_on());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(75 * MATH_US_PER_MS, context->ladder.budget_us);

    TEST_ASSERT_TRUE(render_lab_dynres_set_on(false));
    TEST_ASSERT_FALSE(render_lab_dynres_on());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_FIXED, context->policy);
    fixture();
}

/* The UPSCALE choice is the context's filter at the next raster apply, and a
 * console SET shows as the menu's choice. */
static void
test_upscale_menu_choice_sets_the_context_filter(void) {
    fixture();
    int count = 0;
    const char* const* names = render_lab_upscale_names(&count);
    TEST_ASSERT_EQUAL_INT(UPSCALE_FILTER_SURFACE + 1, count);
    TEST_ASSERT_EQUAL_STRING("SHARP", names[UPSCALE_FILTER_NEAREST]);
    TEST_ASSERT_EQUAL_STRING("SMOOTH", names[UPSCALE_FILTER_SURFACE]);
    TEST_ASSERT_EQUAL_INT(UPSCALE_FILTER_NEAREST, render_lab_upscale_selected());

    TEST_ASSERT_TRUE(render_lab_upscale_select(UPSCALE_FILTER_SURFACE));
    render_lab_apply_raster_settings();
    TEST_ASSERT_EQUAL_INT(UPSCALE_FILTER_SURFACE, render_context_upscale_filter(render_context_main()));
    TEST_ASSERT_FALSE(render_lab_upscale_select(count));
    TEST_ASSERT_EQUAL_INT(UPSCALE_FILTER_SURFACE, render_lab_upscale_selected());

    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, upscale), UPSCALE_FILTER_NEAREST));
    TEST_ASSERT_EQUAL_INT(UPSCALE_FILTER_NEAREST, render_lab_upscale_selected());
    render_lab_apply_raster_settings();
    TEST_ASSERT_EQUAL_INT(UPSCALE_FILTER_NEAREST, render_context_upscale_filter(render_context_main()));
    fixture();
}

/* Apply puts the picked preset's small fill in the main context's settings
 * and leaves its other fields as they were. */
static void
test_apply_camera_settings_hands_the_picked_small_fill_to_the_context(void) {
    fixture();
    render_context_t* context = render_context_main();
    const render_settings_t before = *render_context_settings(context);
    render_settings_t marked = before;
    marked.cull = !before.cull;
    marked.lod.pick_every = before.lod.pick_every + 1;
    render_context_set_settings(context, &marked);
    for (int preset = 1; preset <= 2; preset++) {
        TEST_ASSERT_TRUE(render_lab_small_fill_select(preset));
        (void)render_lab_apply_camera_settings(ENTITY_NONE);
        const render_settings_t after = *render_context_settings(context);
        TEST_ASSERT_EQUAL_INT(render_lab_small_fill().ceiling, after.small_fill.ceiling);
        TEST_ASSERT_EQUAL_INT(render_lab_small_fill().gate, after.small_fill.gate);
        TEST_ASSERT_EQUAL_INT(render_lab_small_fill().style, after.small_fill.style);
        TEST_ASSERT_EQUAL_INT(preset == 1 ? SMALL_FILL_FLAT : SMALL_FILL_DITHER, after.small_fill.style);
        TEST_ASSERT_EQUAL_INT(marked.cull, after.cull);
        TEST_ASSERT_EQUAL_INT(marked.lod.pick_every, after.lod.pick_every);
    }
    render_context_set_settings(context, &before);
    fixture();
}

#ifndef ESP_PLATFORM
/* A scene that generates frames, so the menu draws its FRAME GEN row too. */
static const lab_scene_t generating = {.name = "Generating", .generates_frames = true};

static const lab_scene_t still = {.name = "Still"};

/* One menu frame for `showing` under `input`; FRAME GEN's rectangle in `row`, false when it is not drawn. */
static bool
frame_gen_menu_frame(const lab_scene_t* showing, const input_t* input, recti_t* row) {
    ui_begin(input);
    const lab_menu_screen_state_t state = {.showing = showing, .settings = render_lab_settings_draw};
    (void)lab_menu_screen_draw(&state);
    ui_probe_end_frame();
    return ui_probe_widget(RENDER_LAB_FRAME_GEN_LABEL, row);
}

/* A tap on the FRAME GEN row, as the board's touch reaches the menu. */
static void
tap_frame_gen(const affine2x_t* transform, recti_t row) {
    const vec2i_t point = affine2x_map_pixel(transform, (vec2i_t){row.x + (row.w / 2), row.y + (row.h / 2)});
    input_t touch = {.down = true, .pressed = true, .point = point};
    recti_t drawn;
    (void)frame_gen_menu_frame(&generating, &touch, &drawn);
    touch.pressed = false;
    (void)frame_gen_menu_frame(&generating, &touch, &drawn);
    touch.down = false;
    touch.released = true;
    (void)frame_gen_menu_frame(&generating, &touch, &drawn);
}

/* FRAME GEN shows for a scene that generates frames and not for one that
 * does not; a tap turns render_lab.framegen on, and the next tap off. */
static void
test_frame_gen_row_shows_and_toggles_for_a_generating_scene(void) {
    fixture();
    ui_probe_widgets_start();
    suite_set_test_cleanup(ui_probe_widgets_stop);
    ui_init();
    const affine2x_t transform = ui_transform_quarter_turn(DISPLAY_LANDSCAPE, GFX_WIDTH, GFX_HEIGHT);
    ui_set_transform(transform);
    const input_t idle = {0};
    recti_t row;
    TEST_ASSERT_FALSE(frame_gen_menu_frame(&still, &idle, &row));
    TEST_ASSERT_EQUAL_INT(0, ui_probe_text_count(RENDER_LAB_FRAME_GEN_LABEL));
    TEST_ASSERT_TRUE(frame_gen_menu_frame(&generating, &idle, &row));
    TEST_ASSERT_EQUAL_INT(1, ui_probe_text_count(RENDER_LAB_FRAME_GEN_LABEL));
    TEST_ASSERT_FALSE(render_lab_framegen_on());
    tap_frame_gen(&transform, row);
    TEST_ASSERT_TRUE(render_lab_framegen_on());
    tap_frame_gen(&transform, row);
    TEST_ASSERT_FALSE(render_lab_framegen_on());
    ui_probe_widgets_stop();
    suite_clear_test_cleanup();
    fixture();
}

/* Every preset label, shown as the chosen one, fits the SMALL FILL
 * dropdown's label room in both orientations. */
static void
test_small_fill_labels_fit_their_dropdown(void) {
    static const int quarters[] = {DISPLAY_PORTRAIT, DISPLAY_LANDSCAPE};
    int count = 0;
    const char* const* names = render_lab_small_fill_names(&count);
    ui_probe_widgets_start();
    for (size_t q = 0; q < sizeof quarters / sizeof quarters[0]; q++) {
        ui_init();
        ui_set_transform(ui_transform_quarter_turn(quarters[q], GFX_WIDTH, GFX_HEIGHT));
        for (int i = 0; i < count; i++) {
            TEST_ASSERT_TRUE(render_lab_small_fill_select(i));
            const input_t input = {0};
            ui_begin(&input);
            const lab_menu_screen_state_t state = {.settings = render_lab_settings_draw, .showing = &generating};
            (void)lab_menu_screen_draw(&state);
            ui_probe_end_frame();
            recti_t dropdown;
            recti_t drawn;
            TEST_ASSERT_TRUE(ui_probe_widget(RENDER_LAB_SMALL_FILL_LABEL, &dropdown));
            TEST_ASSERT_TRUE_MESSAGE(ui_probe_text(names[i], &drawn), names[i]);
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(ui_probe_dropdown_label_width(dropdown.w, false, NULL), drawn.w,
                                                  names[i]);
        }
    }
    ui_probe_widgets_stop();
    TEST_ASSERT_TRUE(render_lab_small_fill_select(0));
    fixture();
}
#endif

#ifdef DEVICE_BUILD
/* The menu offers FRAME GEN for exactly the scenes that generate frames, drawn
 * as the lab draws it: the scene's rows, then the lab's for that scene. */
static void
test_frame_gen_row_shows_for_every_scene_that_generates_frames(void) {
    fixture();
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(DISPLAY_LANDSCAPE, GFX_WIDTH, GFX_HEIGHT));
    for (int i = 0; i < render_lab.scene_count; i++) {
        const lab_scene_t* scene = render_lab.scenes[i];
        const input_t input = {0};
        ui_begin(&input);
        const lab_menu_screen_state_t state = {.showing = scene, .settings = render_lab.hooks.settings};
        (void)lab_menu_screen_draw(&state);
        ui_probe_end_frame();
        TEST_ASSERT_EQUAL_INT_MESSAGE(scene->generates_frames, ui_probe_text_count(RENDER_LAB_FRAME_GEN_LABEL),
                                      scene->key);
    }
    fixture();
}
#endif

void
run_render_size_suite(void) {
    RUN_TEST(test_dynamic_resolution_toggle_moves_the_budget_into_the_context);
    RUN_TEST(test_small_fill_menu_choice_moves_the_tunables);
    RUN_TEST(test_upscale_menu_choice_sets_the_context_filter);
#ifdef DEVICE_BUILD
    RUN_TEST(test_frame_gen_row_shows_for_every_scene_that_generates_frames);
#endif
    RUN_TEST(test_apply_camera_settings_hands_the_picked_small_fill_to_the_context);
#ifndef ESP_PLATFORM
    RUN_TEST(test_small_fill_labels_fit_their_dropdown);
    RUN_TEST(test_frame_gen_row_shows_and_toggles_for_a_generating_scene);
#endif
    RUN_TEST(test_small_fill_ceiling_range);
    RUN_TEST(test_render_size_and_percent_follow_and_clamp);
}

SUITE_REGISTER(run_render_size_suite);
