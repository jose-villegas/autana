/*
 * Portable suite: measures the peak bytes this app's two microui screens'
 * command list reaches, driving the REAL render_lab_hud_screen_draw()/
 * render_lab_menu_screen_draw() against a real microui (ui_init() +
 * ui_begin(), the same calls app_render_lab.c makes) rather than a
 * hand-mirrored reconstruction - see docs/Building-a-Screen.md.
 */

#include "suites.h"
#include "ui_budget.h"
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
#include "draw/draw.h"
#include "test_cleanup.h"

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
    render_lab_hud_screen_draw(ui_context(), &state);
    ui_budget_assert("render lab HUD");
}

static void
test_menu_screen_command_list_fits_budget(void) {
    fixture();

    const render_lab_menu_screen_state_t state = {
        .scene_name = "Sponza Fitted Full",
    };

    const input_t input = {0};
    ui_begin(&input);
    render_lab_menu_screen_draw(ui_context(), &state, 16);
    ui_budget_assert("render lab menu");
}

#define BUDGET_CLIP_COUNT ((GFX_HEIGHT / UI_ROW_HEIGHT) + 1)
#define BUDGET_FRAME_MS   16

typedef struct {
    render_lab_clips_t choice;
    ui_dropdown_item_t items[BUDGET_CLIP_COUNT];
    char names[BUDGET_CLIP_COUNT][ASSET_NAME_MAX];
    mu_Rect anchor;
    bool open;
} menu_budget_fixture_t;

static menu_budget_fixture_t* menu_budget;

static void
release_menu_budget(void) {
    memory_free(menu_budget);
    menu_budget = NULL;
}

static void
budget_controls(mu_Context* ctx, ui_flow_t* flow, int width) {
    menu_budget->anchor =
        (mu_Rect){(flow->canvas_w - width) / 2, flow->y + draw_text_height() + flow->gap, width, UI_ROW_HEIGHT};
    render_lab_clips_draw(ctx, flow, width, &menu_budget->choice);
    menu_budget->open = ui_dropdown_is_open(ctx, "clip");
}

static void
budget_menu_frame(const input_t* input) {
    ui_begin(input);
    const render_lab_menu_screen_state_t state = {.scene_name = menu_budget->names[0], .controls = budget_controls};
    (void)render_lab_menu_screen_draw(ui_context(), &state, BUDGET_FRAME_MS);
    ui_budget_assert("animated menu with clip list");
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
        menu_budget->items[i].label = menu_budget->names[i];
    }
    menu_budget->choice = (render_lab_clips_t){
        .items = menu_budget->items, .count = BUDGET_CLIP_COUNT, .blend_ms = RENDER_LAB_BLEND_MAX_MS};
    const input_t idle = {0};
    budget_menu_frame(&idle);
    budget_menu_frame(&idle);
    input_t tap = {.down = true, .pressed = true};
    ui_transform_point(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT),
                       menu_budget->anchor.x + (menu_budget->anchor.w / 2),
                       menu_budget->anchor.y + (menu_budget->anchor.h / 2), &tap.x, &tap.y);
    budget_menu_frame(&tap);
    tap.pressed = false;
    budget_menu_frame(&tap);
    tap.down = false;
    tap.released = true;
    budget_menu_frame(&tap);
    budget_menu_frame(&idle);
    TEST_ASSERT_TRUE(menu_budget->open);
}

void
run_render_lab_command_list_budget_suite(void) {
    RUN_TEST(test_menu_with_full_clip_list_and_long_names_fits_budget);
    RUN_TEST(test_hud_screen_command_list_fits_budget);
    RUN_TEST(test_menu_screen_command_list_fits_budget);
}

SUITE_REGISTER(run_render_lab_command_list_budget_suite);
