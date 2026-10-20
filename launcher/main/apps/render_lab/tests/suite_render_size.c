#include <stdio.h>
#include <string.h>
#include "apps/render_lab/render_lab_view.h"

#include "gfx/gfx.h"
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
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_STRING("CUSTOM", names[count - 1]);
    TEST_ASSERT_EQUAL_INT(0, render_lab_small_fill_selected());

    render_lab_small_fill_select(1);
    TEST_ASSERT_EQUAL_STRING("FAST", names[render_lab_small_fill_selected()]);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_FAST, render_lab_small_fill().ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_GATE_FAST, render_lab_small_fill().gate);

    size_command("SET render_lab.small_fill_gate 3");
    TEST_ASSERT_EQUAL_INT(count - 1, render_lab_small_fill_selected());
    render_lab_small_fill_select(count - 1);
    TEST_ASSERT_EQUAL_INT(count - 1, render_lab_small_fill_selected());

    render_lab_small_fill_select(0);
    TEST_ASSERT_EQUAL_INT(0, render_lab_small_fill_selected());
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_CEILING_SMOOTH, render_lab_small_fill().ceiling);
    fixture();
}

void
run_render_size_suite(void) {
    RUN_TEST(test_small_fill_menu_choice_moves_the_tunables);
    RUN_TEST(test_small_fill_ceiling_range);
    RUN_TEST(test_render_size_and_percent_follow_and_clamp);
}

SUITE_REGISTER(run_render_size_suite);
