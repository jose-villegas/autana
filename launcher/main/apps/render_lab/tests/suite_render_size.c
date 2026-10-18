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
    size_command("RESET render_lab.rows");
}

static void
test_render_size_and_percent_follow_and_clamp(void) {
    fixture();
    const resolution_step_t defaults = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_INT(184, defaults.width);
    TEST_ASSERT_EQUAL_INT(224, defaults.height);

    static const struct {
        const char* scale;
        const char* rows;
        int width_percent, height_percent;
    } cases[] = {
        {"SET render_lab.scale 200", "SET render_lab.rows 0", 50, 50},
        {"SET render_lab.scale 400", "SET render_lab.rows 0", 25, 25},
        {"SET render_lab.scale 200", "SET render_lab.rows 400", 50, 25},
        {"SET render_lab.scale 400", "SET render_lab.rows 1", 25, 100},
        {"SET render_lab.scale 100", "SET render_lab.rows 800", 100, 12},
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        size_command(cases[i].scale);
        size_command(cases[i].rows);
        const resolution_step_t percent = render_lab_render_percent();
        const resolution_step_t size = render_lab_render_size(GFX_WIDTH, GFX_HEIGHT);
        TEST_ASSERT_EQUAL_INT(cases[i].width_percent, percent.width);
        TEST_ASSERT_EQUAL_INT(cases[i].height_percent, percent.height);
        TEST_ASSERT_EQUAL_INT(GFX_WIDTH * cases[i].width_percent / RENDER_LAB_SCALE_UNIT, size.width);
        TEST_ASSERT_EQUAL_INT(GFX_HEIGHT * cases[i].height_percent / RENDER_LAB_SCALE_UNIT, size.height);
    }
    fixture();
}

void
run_render_size_suite(void) {
    RUN_TEST(test_render_size_and_percent_follow_and_clamp);
}

SUITE_REGISTER(run_render_size_suite);
