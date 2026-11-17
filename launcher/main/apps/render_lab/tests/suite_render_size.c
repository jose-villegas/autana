#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"

#include "core/tune.h"
#include "display/display.h"
#include "gfx/gfx.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "suites.h"
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
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_NEAREST, render_lab_present());
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, present), 1));
    size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(184, size.width);
    TEST_ASSERT_EQUAL_INT(112, size.height);
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_SCANLINE, render_lab_present());
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, present), 2));
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_DOUBLE, render_lab_present());
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

/* The toggle is the budget: on sets RENDER_LAB_TARGET_MS, any budget a
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
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_TARGET_MS, render_lab_budget_ms());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_PREDICTED, context->policy);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_TARGET_MS * MATH_US_PER_MS, context->ladder.budget_us);
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

#ifndef ESP_PLATFORM
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
            const render_lab_menu_screen_state_t state = {0};
            (void)render_lab_menu_screen_draw(&state);
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

void
run_render_size_suite(void) {
    RUN_TEST(test_dynamic_resolution_toggle_moves_the_budget_into_the_context);
    RUN_TEST(test_small_fill_menu_choice_moves_the_tunables);
    RUN_TEST(test_upscale_menu_choice_sets_the_context_filter);
#ifndef ESP_PLATFORM
    RUN_TEST(test_small_fill_labels_fit_their_dropdown);
#endif
    RUN_TEST(test_small_fill_ceiling_range);
    RUN_TEST(test_render_size_and_percent_follow_and_clamp);
}

SUITE_REGISTER(run_render_size_suite);
