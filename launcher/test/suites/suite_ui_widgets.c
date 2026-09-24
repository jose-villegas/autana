/*
 * Portable suite: ui_widgets - what a tap on each widget reports, what it
 * draws, and where a dropdown's list opens. Driven through the real
 * ui_begin() and microui.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_font_roles.h"
#include "input/input.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

static const ui_theme_t THEME = {
    .panel_face = UI_RGB(0x202020),
    .panel_edge = UI_RGB(0x505050),
    .button_face = UI_RGB(0x303030),
    .accent = UI_RGB(0xE0B040),
    .accent_face = UI_RGB(0xB08020),
    .on_accent = UI_RGB(0x201000),
    .text = UI_RGB(0xE0E0E0),
    .caption = UI_RGB(0x8090A0),
    .muted = UI_RGB(0x707070),
    .text_scale = 2,
    .icon_side = 32,
};

static const mu_Rect BUTTON = {40, 100, 240, 60};

typedef struct {
    bool enabled;
} widget_t;

static bool
widget_frame(const widget_t* w, const input_t* in) {
    ui_begin(in);
    bool hit = false;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const ui_widget_button_t b = {.label = "GO", .enabled = w->enabled};
        hit = ui_icon_button(ui_context(), "button", BUTTON, &b, &THEME);
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
    return hit;
}

static bool
tap(const widget_t* w) {
    const int x = BUTTON.x + BUTTON.w / 2;
    const int y = BUTTON.y + BUTTON.h / 2;
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y};
    const input_t hold = {.down = true, .x = x, .y = y};
    const input_t release = {.released = true, .x = x, .y = y};

    widget_frame(w, &idle);
    widget_frame(w, &idle);
    bool hit = widget_frame(w, &press);
    for (int i = 0; i < 4; i++) {
        hit |= widget_frame(w, &hold);
    }
    hit |= widget_frame(w, &release);
    hit |= widget_frame(w, &idle);
    return hit;
}

static void
fixture(void) {
    ui_init();
    ui_set_transform(ui_transform_identity());
}

static void
test_an_enabled_button_reports_a_tap(void) {
    fixture();
    const widget_t w = {.enabled = true};
    TEST_ASSERT_TRUE(tap(&w));
}

static void
test_a_disabled_button_takes_no_tap(void) {
    fixture();
    const widget_t w = {.enabled = false};
    TEST_ASSERT_FALSE(tap(&w));
}

static void
test_an_icon_leaves_less_room_for_the_label(void) {
    const int bare = ui_icon_button_label_width(200, false, &THEME);
    const int with_icon = ui_icon_button_label_width(200, true, &THEME);
    TEST_ASSERT_LESS_THAN_INT(200, bare);
    TEST_ASSERT_LESS_OR_EQUAL_INT(bare - THEME.icon_side, with_icon);
}

static mu_Vec2
text_pos_in(mu_Rect r, const char* str, ui_align_t align) {
    const input_t idle = {0};
    ui_begin(&idle);
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        ui_text_in(ui_context(), r, str, THEME.text, 2, align);
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_TEXT) {
            return cmd->text.pos;
        }
    }
    TEST_FAIL_MESSAGE("no text drawn");
    return mu_vec2(0, 0);
}

static void
test_text_aligns_to_either_edge_or_the_centre(void) {
    fixture();
    const mu_Rect r = {20, 40, 200, 30};
    const int w = gfx_font_text_width(gfx_font_ui(), "ABC", -1, 2);

    TEST_ASSERT_EQUAL_INT(r.x, text_pos_in(r, "ABC", UI_ALIGN_LEFT).x);
    TEST_ASSERT_EQUAL_INT(r.x + r.w - w, text_pos_in(r, "ABC", UI_ALIGN_RIGHT).x);
    TEST_ASSERT_EQUAL_INT(r.x + (r.w - w) / 2, text_pos_in(r, "ABC", UI_ALIGN_CENTRE).x);
}

static void
test_a_list_goes_below_its_dropdown_when_it_fits(void) {
    const mu_Rect anchor = {20, 100, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 5, 44, 448, 16);
    TEST_ASSERT_GREATER_THAN_INT(anchor.y + anchor.h - 1, list.y);
    TEST_ASSERT_EQUAL_INT(5 * 44, list.h);
    TEST_ASSERT_EQUAL_INT(anchor.x, list.x);
    TEST_ASSERT_EQUAL_INT(anchor.w, list.w);
}

static void
test_a_list_grows_upward_from_a_dropdown_near_the_bottom(void) {
    const mu_Rect anchor = {20, 340, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 5, 44, 448, 16);
    TEST_ASSERT_LESS_OR_EQUAL_INT(anchor.y, list.y + list.h);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(16, list.y);
}

static bool
overlaps(mu_Rect a, mu_Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void
test_a_list_too_tall_for_either_side_fills_the_roomier_one(void) {
    const mu_Rect anchor = {20, 120, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 5, 44, 300, 16);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(anchor.y + anchor.h - 1, list.y, "more room below than above");
    TEST_ASSERT_EQUAL_INT(300 - 16, list.y + list.h);
    TEST_ASSERT_FALSE(overlaps(list, anchor));

    const mu_Rect low = {20, 200, 200, 44};
    const mu_Rect up = ui_dropdown_list_rect(low, 5, 44, 300, 16);
    TEST_ASSERT_EQUAL_INT(16, up.y);
    TEST_ASSERT_FALSE(overlaps(up, low));
}

static void
test_a_list_taller_than_the_screen_stays_inside_it(void) {
    const mu_Rect anchor = {20, 60, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 10, 44, 300, 16);
    TEST_ASSERT_LESS_THAN_INT(10 * 44, list.h);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(16, list.y);
    TEST_ASSERT_LESS_OR_EQUAL_INT(300 - 16, list.y + list.h);
    TEST_ASSERT_FALSE(overlaps(list, anchor));
}

static void
test_a_list_opens_scrolled_to_its_current_item(void) {
    TEST_ASSERT_EQUAL_INT(0, ui_dropdown_list_scroll(0, 10, 44, 268));
    TEST_ASSERT_EQUAL_INT_MESSAGE(10 * 44 - 268, ui_dropdown_list_scroll(9, 10, 44, 268), "never past the last row");
    const int middle = ui_dropdown_list_scroll(5, 10, 44, 268);
    TEST_ASSERT_TRUE(5 * 44 >= middle && 5 * 44 + 44 <= middle + 268);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui_dropdown_list_scroll(4, 5, 44, 5 * 44), "a list that fits never scrolls");
}

static const ui_dropdown_item_t ITEMS[] = {{.label = "ZERO"}, {.label = "ONE"}, {.label = "TWO"}};
static const mu_Rect DROPDOWN = {40, 60, 240, 44};

static bool dropdown_open;

/* One frame of a window holding only the dropdown; returns its pick. */
static int
dropdown_frame(const input_t* in, int selected) {
    ui_begin(in);
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        picked = ui_dropdown(ui_context(), "pick", DROPDOWN, ITEMS, 3, selected, &THEME);
        dropdown_open = ui_dropdown_is_open(ui_context(), "pick");
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
    return picked;
}

static int
dropdown_tap(int x, int y, int selected) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y};
    const input_t hold = {.down = true, .x = x, .y = y};
    const input_t release = {.released = true, .x = x, .y = y};
    int picked = dropdown_frame(&press, selected);
    for (int i = 0; i < 4; i++) {
        const int p = dropdown_frame(&hold, selected);
        picked = p >= 0 ? p : picked;
    }
    const int p = dropdown_frame(&release, selected);
    picked = p >= 0 ? p : picked;
    dropdown_frame(&idle, selected);
    return picked;
}

static bool
list_open(void) {
    return dropdown_open;
}

static mu_Rect
open_list_rect(void) {
    return ui_dropdown_list_rect(DROPDOWN, 3, DROPDOWN.h, ui_height(), UI_MARGIN);
}

static void
open_fixture(void) {
    fixture();
    const input_t idle = {0};
    dropdown_frame(&idle, 0);
    dropdown_frame(&idle, 0);
    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(DROPDOWN.x + 20, DROPDOWN.y + 20, 0));
    TEST_ASSERT_TRUE_MESSAGE(list_open(), "a tap on the dropdown opens its list");
}

static void
test_a_tap_on_a_listed_item_picks_it_and_closes_the_list(void) {
    open_fixture();
    const mu_Rect list = open_list_rect();
    TEST_ASSERT_EQUAL_INT(2, dropdown_tap(list.x + 20, list.y + 2 * DROPDOWN.h + 20, 0));
    TEST_ASSERT_TRUE_MESSAGE(list_open(), "the list stays up long enough to be seen taking the pick");

    const input_t idle = {0};
    for (int i = 0; i < UI_DROPDOWN_CLOSE_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, dropdown_frame(&idle, 2), "a pick is reported once");
    }
    TEST_ASSERT_FALSE(list_open());
}

static void
test_a_tap_elsewhere_closes_the_list_without_a_pick(void) {
    open_fixture();
    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(5, ui_height() - 5, 0));
    TEST_ASSERT_FALSE(list_open());
}

/* The open list's own window, found by the rect it was placed at. */
static const mu_Container*
open_list_window(void) {
    const mu_Rect want = open_list_rect();
    for (int i = 0; i < ui_context()->root_list.idx; i++) {
        const mu_Container* cnt = ui_context()->root_list.items[i];
        if (cnt->rect.y == want.y && cnt->rect.h == want.h && cnt->rect.x == want.x) {
            return cnt;
        }
    }
    TEST_FAIL_MESSAGE("no window at the open list's rect");
    return NULL;
}

/* microui narrows a window's body for a scrollbar only when it scrolls. */
static void
test_a_list_that_fits_does_not_scroll(void) {
    open_fixture();
    const mu_Container* list = open_list_window();
    TEST_ASSERT_EQUAL_INT_MESSAGE(list->rect.w, list->body.w, "a list that fits must not grow a scrollbar");
    TEST_ASSERT_EQUAL_INT(list->rect.h, list->body.h);
}

static uint64_t
canvas_hash(const char* name) {
    const mu_Container* cnt = mu_get_container(ui_context(), name);
    const unsigned char* p = (const unsigned char*)cnt->head + cnt->head->base.size;
    uint64_t h = 1469598103934665603ull;
    for (; p < (const unsigned char*)cnt->tail; p++) {
        h = (h ^ *p) * 1099511628211ull;
    }
    return h;
}

/* Nothing repaints the rect a closed list leaves, so the screen under it
 * must change its own drawing when the list opens or closes. */
static void
test_opening_the_list_changes_the_screen_under_it(void) {
    fixture();
    const input_t idle = {0};
    dropdown_frame(&idle, 0);
    const uint64_t closed = canvas_hash("Widgets");
    open_fixture();
    dropdown_frame(&idle, 0);
    TEST_ASSERT_TRUE(list_open());
    TEST_ASSERT_NOT_EQUAL(closed, canvas_hash("Widgets"));
}

static const ui_dropdown_item_t MANY[] = {{.label = "0"}, {.label = "1"}, {.label = "2"}, {.label = "3"},
                                          {.label = "4"}, {.label = "5"}, {.label = "6"}, {.label = "7"},
                                          {.label = "8"}, {.label = "9"}};
#define MANY_COUNT ((int)(sizeof MANY / sizeof MANY[0]))

static int
many_frame(const input_t* in, int selected) {
    ui_begin(in);
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        picked = ui_dropdown(ui_context(), "many", DROPDOWN, MANY, MANY_COUNT, selected, &THEME);
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
    return picked;
}

static int
many_tap(int x, int y, int selected) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y};
    const input_t hold = {.down = true, .x = x, .y = y};
    const input_t release = {.released = true, .x = x, .y = y};
    int picked = many_frame(&press, selected);
    for (int i = 0; i < 4; i++) {
        const int p = many_frame(&hold, selected);
        picked = p >= 0 ? p : picked;
    }
    const int p = many_frame(&release, selected);
    picked = p >= 0 ? p : picked;
    many_frame(&idle, selected);
    return picked;
}

/* Ten rows cannot fit; opened on the last, the list must show it, and a
 * tap where it shows must pick it. */
static void
test_the_last_row_of_a_list_taller_than_the_screen_can_be_picked(void) {
    fixture();
    const input_t idle = {0};
    many_frame(&idle, MANY_COUNT - 1);
    many_frame(&idle, MANY_COUNT - 1);
    many_tap(DROPDOWN.x + 20, DROPDOWN.y + 20, MANY_COUNT - 1);

    const mu_Rect list = ui_dropdown_list_rect(DROPDOWN, MANY_COUNT, DROPDOWN.h, ui_height(), UI_MARGIN);
    TEST_ASSERT_LESS_THAN_INT(MANY_COUNT * DROPDOWN.h, list.h);
    const int scroll = ui_dropdown_list_scroll(MANY_COUNT - 1, MANY_COUNT, DROPDOWN.h, list.h);
    const int last_y = list.y + (MANY_COUNT - 1) * DROPDOWN.h - scroll;
    TEST_ASSERT_LESS_OR_EQUAL_INT(list.y + list.h, last_y + DROPDOWN.h);
    TEST_ASSERT_EQUAL_INT(MANY_COUNT - 1, many_tap(list.x + 20, last_y + DROPDOWN.h / 2, MANY_COUNT - 1));
}

/* A finger resting on a button before ui_pointer lands its press - which
 * is all a drag across a list ever is - must not draw it pressed; the
 * press itself must. */
static void
test_only_a_real_press_draws_a_button_pressed(void) {
    fixture();
    const widget_t w = {.enabled = true};
    const input_t idle = {0};
    widget_frame(&w, &idle);
    widget_frame(&w, &idle);
    const uint64_t at_rest = canvas_hash("Widgets");

    const int x = BUTTON.x + BUTTON.w / 2;
    const int y = BUTTON.y + BUTTON.h / 2;
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y};
    const input_t hold = {.down = true, .x = x, .y = y};
    widget_frame(&w, &press);
    widget_frame(&w, &hold);
    TEST_ASSERT_EQUAL_MESSAGE(at_rest, canvas_hash("Widgets"), "hovered but not yet pressed must look at rest");

    widget_frame(&w, &hold);
    widget_frame(&w, &hold);
    const uint64_t pressed = canvas_hash("Widgets");

    const input_t release = {.released = true, .x = x, .y = y};
    widget_frame(&w, &release);
    widget_frame(&w, &idle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(at_rest, pressed, "a landed press must look pressed");
}

/* One frame of the same window with the dropdown left out, as a screen
 * that hides it does. */
static void
frame_without_dropdown(void) {
    const input_t idle = {0};
    ui_begin(&idle);
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
}

static void
test_a_list_its_dropdown_stopped_drawing_is_closed_when_it_returns(void) {
    open_fixture();
    frame_without_dropdown();

    const input_t idle = {0};
    dropdown_frame(&idle, 0);
    TEST_ASSERT_FALSE_MESSAGE(list_open(), "a list left open by a screen that moved on must not reappear");
}

void
run_ui_widgets_suite(void) {
    RUN_TEST(test_an_enabled_button_reports_a_tap);
    RUN_TEST(test_a_disabled_button_takes_no_tap);
    RUN_TEST(test_an_icon_leaves_less_room_for_the_label);
    RUN_TEST(test_text_aligns_to_either_edge_or_the_centre);
    RUN_TEST(test_a_list_goes_below_its_dropdown_when_it_fits);
    RUN_TEST(test_a_list_grows_upward_from_a_dropdown_near_the_bottom);
    RUN_TEST(test_a_list_too_tall_for_either_side_fills_the_roomier_one);
    RUN_TEST(test_a_tap_on_a_listed_item_picks_it_and_closes_the_list);
    RUN_TEST(test_a_tap_elsewhere_closes_the_list_without_a_pick);
    RUN_TEST(test_opening_the_list_changes_the_screen_under_it);
    RUN_TEST(test_a_list_that_fits_does_not_scroll);
    RUN_TEST(test_only_a_real_press_draws_a_button_pressed);
    RUN_TEST(test_a_list_its_dropdown_stopped_drawing_is_closed_when_it_returns);
    RUN_TEST(test_a_list_taller_than_the_screen_stays_inside_it);
    RUN_TEST(test_a_list_opens_scrolled_to_its_current_item);
    RUN_TEST(test_the_last_row_of_a_list_taller_than_the_screen_can_be_picked);
}

SUITE_REGISTER(run_ui_widgets_suite);
