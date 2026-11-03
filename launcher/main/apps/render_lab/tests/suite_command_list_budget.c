/*
 * Portable suite: measures the peak bytes this app's two screens' command
 * list reaches, driving the REAL render_lab_hud_screen_draw()/
 * render_lab_menu_screen_draw() through ui_begin(), the same calls
 * app_render_lab.c makes, rather than a hand-mirrored reconstruction - see
 * docs/ui/Building-a-Screen.md.
 */

#include "suites.h"
#include "unity.h"

#include <string.h>

#include "app/app.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_clips.h"
#include "apps/render_lab/ui/render_lab_hud_screen.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"
#include "core/memory.h"
#include "testkit/cleanup.h"
#include "ui_probe.h"

/* Landscape (448x368 logical) - this app's shipping orientation. */
static void
fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
}

static void
test_hud_screen_command_list_fits_budget(void) {
    fixture();

    char status[RENDER_LAB_STATUS_LEN];
    memset(status, 'x', sizeof status - 1);
    status[sizeof status - 1] = '\0';
    const render_lab_hud_screen_state_t state = {
        .fps_value = 999.9F,
        .scene_title = "Sponza Fitted Full",
        .scene_title_alpha = 255,
        .status = status,
    };

    const input_t input = {0};
    ui_begin(&input);
    render_lab_hud_screen_draw(&state);
    ui_probe_budget_assert("render lab HUD");
}

static void
test_menu_screen_command_list_fits_budget(void) {
    fixture();

    const char* const families[] = {"Sponza", "Capybara"};
    const render_lab_menu_screen_state_t state = {.families = families, .family_count = 2};

    const input_t input = {0};
    ui_begin(&input);
    render_lab_menu_screen_draw(&state);
    ui_probe_budget_assert("render lab menu");
}

#define BUDGET_CLIP_COUNT ((GFX_HEIGHT / UI_ROW_HEIGHT) + 1)

typedef struct {
    render_lab_clips_t choice;
    const char* items[BUDGET_CLIP_COUNT];
    char names[BUDGET_CLIP_COUNT][ASSET_NAME_MAX];
} menu_budget_fixture_t;

static menu_budget_fixture_t* menu_budget;

static void
release_menu_budget(void) {
    memory_free(menu_budget);
    menu_budget = NULL;
}

static void
budget_controls(int width) {
    render_lab_clips_draw(width, &menu_budget->choice);
}

static void
budget_menu_frame(const input_t* input) {
    ui_begin(input);
    const render_lab_menu_screen_state_t state = {.controls = budget_controls};
    (void)render_lab_menu_screen_draw(&state);
    ui_probe_budget_assert("animated menu with clip list");
}

static void
test_menu_with_full_clip_list_and_long_names_fits_budget(void) {
    fixture();
    menu_budget = memory_alloc(sizeof *menu_budget, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(menu_budget);
    suite_set_test_cleanup(release_menu_budget);
    *menu_budget = (menu_budget_fixture_t){0};
    for (int i = 0; i < BUDGET_CLIP_COUNT; i++) {
        memset(menu_budget->names[i], 'a' + i, ASSET_NAME_MAX - 1);
        menu_budget->names[i][ASSET_NAME_MAX - 1] = '\0';
        menu_budget->items[i] = menu_budget->names[i];
    }
    menu_budget->choice = (render_lab_clips_t){
        .items = menu_budget->items, .count = BUDGET_CLIP_COUNT, .blend_ms = RENDER_LAB_BLEND_MAX_MS};
    const input_t idle = {0};
    budget_menu_frame(&idle);
    budget_menu_frame(&idle);
    /* The dropdown shows the chosen name, centred on the canvas like the
     * menu's other rows. */
    recti_t chosen;
    TEST_ASSERT_TRUE(ui_probe_text(menu_budget->names[0], &chosen));
    input_t tap = {.down = true, .pressed = true};
    const affine2x_t transform = ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT);
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){ui_width() / 2, chosen.y + (chosen.h / 2)});
    tap.point = mapped_pixel;
    budget_menu_frame(&tap);
    tap.pressed = false;
    budget_menu_frame(&tap);
    tap.down = false;
    tap.released = true;
    budget_menu_frame(&tap);
    budget_menu_frame(&idle);
    recti_t rows;
    int scroll;
    TEST_ASSERT_TRUE(ui_probe_list(&rows, &scroll));
}

void
run_render_lab_command_budget_suite(void) {
    RUN_TEST(test_menu_with_full_clip_list_and_long_names_fits_budget);
    RUN_TEST(test_hud_screen_command_list_fits_budget);
    RUN_TEST(test_menu_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_render_lab_command_budget_suite);
