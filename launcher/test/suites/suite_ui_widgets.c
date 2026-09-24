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
#include "ui/ui_internal.h"
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

/* The exact pixel where ui_dropdown_list_rect() switches sides: a list
 * exactly the room below still fits below, one pixel taller does not and
 * goes above instead - and the same edge holds for the room above, since
 * this anchor leaves far more room above than below. */
static void
test_a_list_switches_sides_at_the_exact_pixel_of_room(void) {
    const mu_Rect anchor = {20, 300, 200, 44};
    const int margin = 16;
    const int canvas_h = 600;
    const int below = anchor.y + anchor.h + 4; /* LIST_GAP, mirrored from ui_widgets.c */
    const int room_below = canvas_h - margin - below;
    const int room_above = anchor.y - 4 - margin;
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(room_below, room_above, "this anchor must leave more room above than below");

    const mu_Rect fits_below = ui_dropdown_list_rect(anchor, room_below, 1, canvas_h, margin);
    TEST_ASSERT_EQUAL_INT_MESSAGE(below, fits_below.y, "a list exactly the room below must still fit below");

    const mu_Rect one_taller = ui_dropdown_list_rect(anchor, room_below + 1, 1, canvas_h, margin);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(below, one_taller.y, "one pixel taller than the room below must not fit below");
    TEST_ASSERT_EQUAL_INT_MESSAGE(anchor.y - 4 - one_taller.h, one_taller.y,
                                  "it must grow upward instead, since there is room");

    const mu_Rect fits_above = ui_dropdown_list_rect(anchor, room_above, 1, canvas_h, margin);
    TEST_ASSERT_EQUAL_INT_MESSAGE(margin, fits_above.y, "a list exactly the room above must still fit above");
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

/* ui_dropdown_list_scroll() must CENTRE the row, not merely bring it on
 * screen - a value only far enough to show the row would satisfy the test
 * above too, so this checks the row's own centre lands on the list's. */
static void
test_a_list_opens_scrolled_so_the_row_sits_centred(void) {
    const int row_h = 44;
    const int list_h = 268;
    const int scroll = ui_dropdown_list_scroll(5, 20, row_h, list_h);
    const int row_top_in_list = 5 * row_h - scroll;
    TEST_ASSERT_EQUAL_INT_MESSAGE(list_h / 2, row_top_in_list + row_h / 2,
                                  "the selected row must be centred in the list, not merely visible");
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

/* A picked list must not stay open forever: closed within
 * UI_DROPDOWN_CLOSE_FRAMES finger-up frames of the pick, not merely
 * "eventually". */
static void
test_the_list_closes_within_the_close_frame_budget(void) {
    open_fixture();
    const mu_Rect list = open_list_rect();
    TEST_ASSERT_EQUAL_INT(2, dropdown_tap(list.x + 20, list.y + 2 * DROPDOWN.h + 20, 0));
    TEST_ASSERT_TRUE(list_open());

    const input_t idle = {0};
    int closed_after = -1;
    for (int i = 0; i < UI_DROPDOWN_CLOSE_FRAMES + 4 && closed_after < 0; i++) {
        dropdown_frame(&idle, 2);
        if (!list_open()) {
            closed_after = i;
        }
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, closed_after, "a picked list must not stay open forever");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(UI_DROPDOWN_CLOSE_FRAMES, closed_after,
                                          "must close within UI_DROPDOWN_CLOSE_FRAMES finger-up frames of the pick");
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

/* Only the rects inside the chevron's own icon box - not a whole-canvas
 * hash, which the dropdown BUTTON's own press bezel also changes on the
 * very frame a held press lands, for a reason unrelated to the chevron. */
static uint64_t
chevron_hash(void) {
    const mu_Rect box = {DROPDOWN.x + DROPDOWN.w - 40, DROPDOWN.y + 8, 32, 28};
    mu_Command* cmd = NULL;
    uint64_t h = 1469598103934665603ull;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type != MU_COMMAND_RECT) {
            continue;
        }
        const mu_Rect r = cmd->rect.rect;
        if (r.x < box.x || r.y < box.y || r.x + r.w > box.x + box.w || r.y + r.h > box.y + box.h) {
            continue;
        }
        const unsigned char* p = (const unsigned char*)&cmd->rect;
        for (size_t i = 0; i < sizeof cmd->rect; i++) {
            h = (h ^ p[i]) * 1099511628211ull;
        }
    }
    return h;
}

/* was_open is read at the top of ui_dropdown(), before this same frame's own
 * click can open the list - so the very frame the list's own open flag
 * flips true must still draw the closed chevron, changing only on the frame
 * after. Finds that transition frame rather than assuming which one it is. */
static void
test_the_chevron_lags_the_lists_own_open_flag_by_one_frame(void) {
    fixture();
    const input_t idle = {0};
    dropdown_frame(&idle, 0);
    dropdown_frame(&idle, 0);
    const uint64_t closed = chevron_hash();

    const int x = DROPDOWN.x + 20;
    const int y = DROPDOWN.y + 20;
    const input_t press = {.down = true, .pressed = true, .x = x, .y = y};
    const input_t hold = {.down = true, .x = x, .y = y};

    dropdown_frame(&press, 0);
    bool checked = false;
    for (int i = 0; i < 8 && !checked; i++) {
        dropdown_frame(&hold, 0);
        if (list_open()) {
            checked = true;
            TEST_ASSERT_EQUAL_MESSAGE(closed, chevron_hash(),
                                      "the chevron must still read closed on the very frame the list opens");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(checked, "the list must open within a few held frames");

    dropdown_frame(&hold, 0);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(closed, chevron_hash(), "the frame after opening must show the open chevron");
}

/* Closing a list - by a pick, or by a tap outside it - must put the
 * dropdown's own drawing back to exactly its pre-open closed state, not
 * merely stop showing the list. `selected` stays 0 through the whole test so
 * the chosen label never itself changes what the canvas would hash to. */
static void
test_closing_the_list_restores_the_closed_drawing(void) {
    fixture();
    const input_t idle = {0};
    dropdown_frame(&idle, 0);
    dropdown_frame(&idle, 0);
    const uint64_t closed = canvas_hash("Widgets");

    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(DROPDOWN.x + 20, DROPDOWN.y + 20, 0));
    TEST_ASSERT_TRUE(list_open());
    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(5, ui_height() - 5, 0));
    TEST_ASSERT_FALSE(list_open());
    dropdown_frame(&idle, 0);
    TEST_ASSERT_EQUAL_MESSAGE(closed, canvas_hash("Widgets"),
                              "an outside-tap close must redraw the dropdown's own closed chevron");

    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(DROPDOWN.x + 20, DROPDOWN.y + 20, 0));
    const mu_Rect list = open_list_rect();
    TEST_ASSERT_EQUAL_INT(2, dropdown_tap(list.x + 20, list.y + 2 * DROPDOWN.h + 20, 0));
    for (int i = 0; i < UI_DROPDOWN_CLOSE_FRAMES; i++) {
        dropdown_frame(&idle, 0);
    }
    TEST_ASSERT_FALSE(list_open());
    TEST_ASSERT_EQUAL_MESSAGE(closed, canvas_hash("Widgets"),
                              "a picked close must also redraw the dropdown's own closed chevron");
}

static const ui_dropdown_item_t MANY[] = {{.label = "0"}, {.label = "1"}, {.label = "2"}, {.label = "3"},
                                          {.label = "4"}, {.label = "5"}, {.label = "6"}, {.label = "7"},
                                          {.label = "8"}, {.label = "9"}};
#define MANY_COUNT ((int)(sizeof MANY / sizeof MANY[0]))

static bool many_open;

static int
many_frame(const input_t* in, int selected) {
    ui_begin(in);
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        picked = ui_dropdown(ui_context(), "many", DROPDOWN, MANY, MANY_COUNT, selected, &THEME);
        many_open = ui_dropdown_is_open(ui_context(), "many");
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
    /* The same report-back ui.c's ui_end() makes after painting a real
     * frame - see ui_internal.h's own comment on ui_pointer_state. */
    ui_pointer_state.over_scrollable = ui_ctx.scroll_target != NULL;
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

/* The "many" dropdown's own open list, found by its rect - it is a nested
 * popup, so a bare mu_get_container(ctx, "many list") hashes to the wrong id
 * outside the window scope it was opened in and silently returns an unrelated,
 * always-empty container instead. */
static int
many_list_scroll(mu_Rect list_rect) {
    for (int i = 0; i < ui_context()->root_list.idx; i++) {
        const mu_Container* cnt = ui_context()->root_list.items[i];
        if (cnt->rect.x == list_rect.x && cnt->rect.y == list_rect.y && cnt->rect.h == list_rect.h) {
            return cnt->scroll.y;
        }
    }
    TEST_FAIL_MESSAGE("no window at the many list's rect");
    return 0;
}

/* A finger already resting at (x, y), optionally with a raw scroll fed via
 * mu_input_scroll() - what replay_pointer_event() does for a resolved
 * UI_POINTER_SCROLL, without depending on which held frame ui_pointer's own
 * drag threshold happens to trip on. */
static int
many_frame_scroll(int x, int y, int selected, int scroll_dy) {
    const input_t held = {.down = true, .x = x, .y = y};
    ui_begin(&held);
    if (scroll_dy != 0) {
        mu_input_scroll(ui_context(), 0, scroll_dy);
    }
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        picked = ui_dropdown(ui_context(), "many", DROPDOWN, MANY, MANY_COUNT, selected, &THEME);
        many_open = ui_dropdown_is_open(ui_context(), "many");
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
    ui_pointer_state.over_scrollable = ui_ctx.scroll_target != NULL;
    return picked;
}

/* A drag-scroll through a tall open list must stick where the finger left
 * it: later frames - held still, or idle once the finger lifts - must not
 * snap the list back to where it opened. */
static void
test_a_drag_scroll_sticks_through_later_frames(void) {
    fixture();
    const input_t idle = {0};
    many_frame(&idle, MANY_COUNT / 2);
    many_frame(&idle, MANY_COUNT / 2);
    many_tap(DROPDOWN.x + 20, DROPDOWN.y + 20, MANY_COUNT / 2);
    TEST_ASSERT_TRUE(many_open);

    const mu_Rect list = ui_dropdown_list_rect(DROPDOWN, MANY_COUNT, DROPDOWN.h, ui_height(), UI_MARGIN);
    const int cx = list.x + list.w / 2;
    const int py = list.y + list.h / 2;

    many_frame_scroll(cx, py, MANY_COUNT / 2, 0);
    many_frame_scroll(cx, py, MANY_COUNT / 2, 20);

    const int dragged = many_list_scroll(list);
    const int opened_scroll = ui_dropdown_list_scroll(MANY_COUNT / 2, MANY_COUNT, DROPDOWN.h, list.h);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(opened_scroll, dragged, "the drag must actually have moved the scroll");

    many_frame_scroll(cx, py, MANY_COUNT / 2, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(dragged, many_list_scroll(list), "holding still must not move the scroll again");

    many_frame(&(input_t){.released = true, .x = cx, .y = py}, MANY_COUNT / 2);
    many_frame(&idle, MANY_COUNT / 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(dragged, many_list_scroll(list),
                                  "a drag-scroll must stick, not snap back to where the list opened");
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

/* A press that lands off the button and only slides onto it afterward is
 * exactly what a drag across a list looks like to a plain button beneath
 * it - it must never draw pressed and never report a click, since the
 * button never actually took focus. */
static void
test_a_press_that_slides_onto_a_button_from_off_it_is_not_a_click(void) {
    fixture();
    const widget_t w = {.enabled = true};
    const input_t idle = {0};
    widget_frame(&w, &idle);
    widget_frame(&w, &idle);
    const uint64_t at_rest = canvas_hash("Widgets");

    const int bx = BUTTON.x + BUTTON.w / 2;
    const int by = BUTTON.y + BUTTON.h / 2;
    const int off_x = BUTTON.x - 40;

    const input_t press_off = {.down = true, .pressed = true, .x = off_x, .y = by};
    const input_t hold_off = {.down = true, .x = off_x, .y = by};
    const input_t hold_on = {.down = true, .x = bx, .y = by};
    const input_t release_on = {.released = true, .x = bx, .y = by};

    bool hit = widget_frame(&w, &press_off);
    hit |= widget_frame(&w, &hold_off);
    hit |= widget_frame(&w, &hold_off);
    for (int i = 0; i < 4; i++) {
        hit |= widget_frame(&w, &hold_on);
    }
    TEST_ASSERT_EQUAL_MESSAGE(at_rest, canvas_hash("Widgets"),
                              "a drag that only slides onto a button must not draw it pressed");
    hit |= widget_frame(&w, &release_on);
    widget_frame(&w, &idle);
    TEST_ASSERT_FALSE_MESSAGE(hit, "a drag that only slides onto a button must not report a click");
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

/* One frame drawing a swatch grid alone, so its command list holds nothing
 * but the grid's own cells. */
static void
swatch_frame(mu_Rect r, const mu_Color* colors, int cols, int rows) {
    const input_t idle = {0};
    ui_begin(&idle);
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        ui_swatch_grid(ui_context(), r, colors, cols, rows);
        mu_end_window(ui_context());
    }
    mu_end(ui_context());
}

/* Every cell of a swatch grid must tile its rect exactly - no gap and no
 * overlap between neighbours, and the grid's own edges must reach the
 * rect's - including a size that does not divide evenly by its column or
 * row count. */
static void
test_ui_swatch_grid_tiles_the_rect_exactly(void) {
    fixture();
    const mu_Rect r = {10, 20, 100, 33};
    const int cols = 3;
    const int rows = 2;
    mu_Color colors[6];
    for (int i = 0; i < 6; i++) {
        colors[i] = ui_rgb((uint32_t)(0x010203 * (i + 1)));
    }
    swatch_frame(r, colors, cols, rows);

    mu_Rect cells[6];
    int n = 0;
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == MU_COMMAND_RECT) {
            TEST_ASSERT_LESS_THAN_INT_MESSAGE(6, n, "the grid must draw exactly one rect per cell");
            cells[n++] = cmd->rect.rect;
        }
    }
    TEST_ASSERT_EQUAL_INT(6, n);

    for (int row = 0; row < rows; row++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(r.x, cells[row * cols].x, "a row must reach the rect's left edge");
        TEST_ASSERT_EQUAL_INT_MESSAGE(r.x + r.w, cells[row * cols + cols - 1].x + cells[row * cols + cols - 1].w,
                                      "a row must reach the rect's right edge");
        for (int col = 0; col + 1 < cols; col++) {
            const mu_Rect a = cells[row * cols + col];
            const mu_Rect b = cells[row * cols + col + 1];
            TEST_ASSERT_EQUAL_INT_MESSAGE(a.x + a.w, b.x, "neighbouring cells must meet with no gap or overlap");
        }
    }
    for (int col = 0; col < cols; col++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(r.y, cells[col].y, "a column must reach the rect's top edge");
        TEST_ASSERT_EQUAL_INT_MESSAGE(r.y + r.h, cells[(rows - 1) * cols + col].y + cells[(rows - 1) * cols + col].h,
                                      "a column must reach the rect's bottom edge");
    }
    for (int row = 0; row + 1 < rows; row++) {
        for (int col = 0; col < cols; col++) {
            const mu_Rect a = cells[row * cols + col];
            const mu_Rect b = cells[(row + 1) * cols + col];
            TEST_ASSERT_EQUAL_INT_MESSAGE(a.y + a.h, b.y, "neighbouring rows must meet with no gap or overlap");
        }
    }
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
    RUN_TEST(test_a_list_switches_sides_at_the_exact_pixel_of_room);
    RUN_TEST(test_a_tap_on_a_listed_item_picks_it_and_closes_the_list);
    RUN_TEST(test_the_list_closes_within_the_close_frame_budget);
    RUN_TEST(test_a_tap_elsewhere_closes_the_list_without_a_pick);
    RUN_TEST(test_opening_the_list_changes_the_screen_under_it);
    RUN_TEST(test_the_chevron_lags_the_lists_own_open_flag_by_one_frame);
    RUN_TEST(test_closing_the_list_restores_the_closed_drawing);
    RUN_TEST(test_a_list_that_fits_does_not_scroll);
    RUN_TEST(test_only_a_real_press_draws_a_button_pressed);
    RUN_TEST(test_a_press_that_slides_onto_a_button_from_off_it_is_not_a_click);
    RUN_TEST(test_a_list_its_dropdown_stopped_drawing_is_closed_when_it_returns);
    RUN_TEST(test_a_list_taller_than_the_screen_stays_inside_it);
    RUN_TEST(test_a_list_opens_scrolled_to_its_current_item);
    RUN_TEST(test_a_list_opens_scrolled_so_the_row_sits_centred);
    RUN_TEST(test_the_last_row_of_a_list_taller_than_the_screen_can_be_picked);
    RUN_TEST(test_a_drag_scroll_sticks_through_later_frames);
    RUN_TEST(test_ui_swatch_grid_tiles_the_rect_exactly);
}

SUITE_REGISTER(run_ui_widgets_suite);
