#include <stdio.h>
#include <string.h>
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"

#include "gfx/gfx.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "services/tune.h"
#include "suites.h"
#include "unity.h"

static bool tune_ok;

static void
size_reply(const char* line) {
    tune_ok = strncmp(line, "TUNE_OK ", sizeof "TUNE_OK " - 1) == 0;
}

static void
size_command(const char* line) {
    tune_ok = false;
    (void)tune_handle_line(line, size_reply);
    TEST_ASSERT_TRUE(tune_ok);
}

static void
fixture(void) {
    size_command("RESET render_lab.scale");
    size_command("RESET render_lab.present");
    size_command("RESET render_lab.small_fill_ceiling");
    size_command("RESET render_lab.small_fill_gate");
    size_command("RESET render_lab.small_fill_style");
    size_command("RESET render_lab.budget");
}

static void
test_render_size_and_percent_follow_and_clamp(void) {
    fixture();
    resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(184, size.width);
    TEST_ASSERT_EQUAL_INT(224, size.height);
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_NEAREST, render_lab_present());
    size_command("SET render_lab.present 1");
    size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(184, size.width);
    TEST_ASSERT_EQUAL_INT(112, size.height);
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_SCANLINE, render_lab_present());
    size_command("SET render_lab.present 2");
    TEST_ASSERT_EQUAL_INT(CAMERA_PRESENT_DOUBLE, render_lab_present());
    tune_ok = false;
    TEST_ASSERT_TRUE(tune_handle_line("SET render_lab.present 3", size_reply));
    TEST_ASSERT_FALSE(tune_ok);
    fixture();
}

static void
test_small_fill_ceiling_range(void) {
    fixture();
    tune_registry_t* registry = tune_shared();
    const char* reset = "RESET render_lab.small_fill_ceiling";
    const char* name = reset + sizeof "RESET " - 1;
    TEST_ASSERT_TRUE(tune_set(registry, name, SMALL_FILL_CEILING_MAX));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_MAX, render_lab_small_fill().ceiling);
    TEST_ASSERT_FALSE(tune_set(registry, name, SMALL_FILL_CEILING_MAX + 1));
    TEST_ASSERT_FALSE(tune_set(registry, name, UINT8_MAX));
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_MAX, render_lab_small_fill().ceiling);
    TEST_ASSERT_TRUE(tune_set(registry, name, 0));
    TEST_ASSERT_EQUAL_INT(0, render_lab_small_fill().ceiling);
    TEST_ASSERT_FALSE(tune_set(registry, name, -1));
    size_command(reset);
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

    size_command("SET render_lab.small_fill_gate 3");
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

/* The toggle is the budget: on sets RENDER_LAB_BUDGET_ON_MS, any budget a
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
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_BUDGET_ON_MS, render_lab_budget_ms());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_PREDICTED, context->policy);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_BUDGET_ON_MS * MATH_US_PER_MS, context->ladder.budget_us);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_LADDER_STEPS, context->ladder.step_count);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_LADDER_HALF, context->predict.step);

    /* A scene switch stops it; the next scene's apply hands it again. */
    render_lab_dynres_stop();
    TEST_ASSERT_EQUAL_INT(RENDER_FIXED, context->policy);
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_PREDICTED, context->policy);

    size_command("SET render_lab.budget 75");
    TEST_ASSERT_TRUE(render_lab_dynres_on());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(75 * MATH_US_PER_MS, context->ladder.budget_us);

    TEST_ASSERT_TRUE(render_lab_dynres_set_on(false));
    TEST_ASSERT_FALSE(render_lab_dynres_on());
    render_lab_dynres_apply();
    TEST_ASSERT_EQUAL_INT(RENDER_FIXED, context->policy);
    fixture();
}

void
run_render_size_suite(void) {
    RUN_TEST(test_dynamic_resolution_toggle_moves_the_budget_into_the_context);
    RUN_TEST(test_small_fill_menu_choice_moves_the_tunables);
    RUN_TEST(test_small_fill_ceiling_range);
    RUN_TEST(test_render_size_and_percent_follow_and_clamp);
}

SUITE_REGISTER(run_render_size_suite);
