/*
 * Portable suite: the HUD's layout and fade, read from the face frame the
 * REAL render_lab_hud_screen_draw() builds. The host renders of the HUD are
 * unpinned (the scene under it is float), so these are what pin it.
 */

#include "suites.h"
#include "unity.h"

#include "apps/render_lab/ui/render_lab_hud_screen.h"
#include "gfx/gfx.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui_probe.h"

#define HUD_TITLE       "Sponza"
#define HUD_STATUS_LONG "12859 tris, 42 meshlets"
#define HUD_FADE        128
#define HUD_FPS_LOW     1.0F
#define HUD_FPS_HIGH    999.9F

/* Portrait: the narrower canvas, where the title meets the corner rows. */
static void
hud_frame(const render_lab_hud_screen_state_t* state) {
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(0, GFX_WIDTH, GFX_HEIGHT));
    ui_begin(NULL);
    render_lab_hud_screen_draw(state);
    ui_end_for_bands(UI_NO_BACKGROUND);
}

static recti_t
drawn(const char* s) {
    recti_t r = {0};
    TEST_ASSERT_TRUE_MESSAGE(ui_probe_text(s, &r), s);
    return r;
}

/* The status line sits under the fps line, and is absent without a status. */
static void
test_status_stacks_under_fps(void) {
    hud_frame(&(render_lab_hud_screen_state_t){
        .fps_value = HUD_FPS_LOW, .scene_title = HUD_TITLE, .status = HUD_STATUS_LONG});
    const recti_t fps = drawn("  1.0 fps");
    const recti_t status = drawn(HUD_STATUS_LONG);
    TEST_ASSERT_EQUAL_INT(fps.x, status.x);
    TEST_ASSERT_GREATER_THAN_INT(recti_bottom(fps), status.y);

    hud_frame(&(render_lab_hud_screen_state_t){.fps_value = HUD_FPS_LOW, .scene_title = HUD_TITLE});
    recti_t none;
    TEST_ASSERT_FALSE(ui_probe_text(HUD_STATUS_LONG, &none));
}

/* The title is centred, keeps its row as the fps reading changes, and drops
 * below a status line that reaches it. */
static void
test_title_row_follows_corner_boxes(void) {
    hud_frame(&(render_lab_hud_screen_state_t){
        .fps_value = HUD_FPS_LOW, .scene_title = HUD_TITLE, .scene_title_alpha = UINT8_MAX});
    const recti_t low = drawn(HUD_TITLE);
    TEST_ASSERT_INT_WITHIN(1, (ui_width() - low.w) / 2, low.x);

    hud_frame(&(render_lab_hud_screen_state_t){
        .fps_value = HUD_FPS_HIGH, .scene_title = HUD_TITLE, .scene_title_alpha = UINT8_MAX});
    TEST_ASSERT_EQUAL_INT(low.y, drawn(HUD_TITLE).y);

    hud_frame(&(render_lab_hud_screen_state_t){
        .fps_value = HUD_FPS_LOW, .scene_title = HUD_TITLE, .scene_title_alpha = UINT8_MAX, .status = HUD_STATUS_LONG});
    TEST_ASSERT_GREATER_THAN_INT(recti_bottom(drawn(HUD_STATUS_LONG)), drawn(HUD_TITLE).y);
}

/* Alpha fades the title's ink alone; 0 draws no title. */
static void
test_title_fades_alone(void) {
    hud_frame(&(render_lab_hud_screen_state_t){
        .fps_value = HUD_FPS_LOW, .scene_title = HUD_TITLE, .scene_title_alpha = HUD_FADE});
    TEST_ASSERT_EQUAL_INT(HUD_FADE, ui_probe_text_alpha(HUD_TITLE));
    TEST_ASSERT_EQUAL_INT(UINT8_MAX, ui_probe_text_alpha("  1.0 fps"));

    hud_frame(&(render_lab_hud_screen_state_t){.fps_value = HUD_FPS_LOW, .scene_title = HUD_TITLE});
    TEST_ASSERT_EQUAL_INT(-1, ui_probe_text_alpha(HUD_TITLE));
}

void
run_render_lab_hud_screen_suite(void) {
    RUN_TEST(test_status_stacks_under_fps);
    RUN_TEST(test_title_row_follows_corner_boxes);
    RUN_TEST(test_title_fades_alone);
}

SUITE_REGISTER(run_render_lab_hud_screen_suite);
