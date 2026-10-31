/*
 * Portable suite: ui_widgets, what a tap on each widget reports, what it
 * draws, and where a dropdown's list opens. Driven through the real
 * ui_begin() and microui.
 */

#include <stdlib.h>
#include <string.h>

#include "hash/name_hash64.h"
#include "suites.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "apps/sand/sand_mode_swatches.h"
#include "apps/sand/sand_palette256.h"
#include "apps/sand/ui/options_screen.h"
#include "apps/sand/ui/sand_icons.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/present/gfx_mode.h"
#ifdef DEVICE_BUILD
#include "esp_log.h"
#endif
#include "draw/draw.h"
#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "image/pixel/rgb565.h"
#include "input/input.h"
#include "ui/input/ui_bridge.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"

static const ui_mu_theme_t THEME = {
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

static void
test_theme_from_style_maps_widget_colors(void) {
    ui_init();
    mu_Style style = *ui_context()->style;
    for (int i = 0; i < MU_COLOR_MAX; i++) {
        style.colors[i] = (mu_Color){(uint8_t)i, (uint8_t)(i + 1), (uint8_t)(i + 2), (uint8_t)(i + 3)};
    }
    const ui_mu_theme_t theme = ui_theme_from_style(&style);
    TEST_ASSERT_EQUAL_MEMORY(&style.colors[MU_COLOR_BUTTON], &theme.button_face, sizeof(mu_Color));
    TEST_ASSERT_EQUAL_MEMORY(&style.colors[MU_COLOR_BUTTONFOCUS], &theme.accent_face, sizeof(mu_Color));
    TEST_ASSERT_EQUAL_MEMORY(&style.colors[MU_COLOR_TEXT], &theme.text, sizeof(mu_Color));
    TEST_ASSERT_EQUAL_MEMORY(&style.colors[MU_COLOR_TEXT], &theme.on_accent, sizeof(mu_Color));
    TEST_ASSERT_EQUAL_INT(DRAW_GLYPH_SCALE, theme.text_scale);
}

static const mu_Rect BUTTON = {40, 100, 240, 60};

typedef struct {
    bool enabled;
} widget_t;

/* What ui_end() makes after a real frame: the pointer's report-back. */
static void
end_frame(void) {
    mu_end(ui_context());
    ui_end_pointer_frame();
}

static bool
widget_frame(const widget_t* w, const input_t* in) {
    ui_begin(in);
    bool hit = false;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        const ui_widget_button_t b = {.label = "GO", .enabled = w->enabled};
        hit = ui_icon_button(ui_context(), "button", BUTTON, &b, &THEME);
        mu_end_window(ui_context());
    }
    end_frame();
    return hit;
}

static bool
plain_button_frame(const input_t* in) {
    ui_begin(in);
    bool hit = false;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_set_next(ui_context(), BUTTON, 0);
        hit = mu_button(ui_context(), "GO") != 0;
        mu_end_window(ui_context());
    }
    end_frame();
    return hit;
}

static void
label_frame(const input_t* in) {
    ui_begin(in);
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_set_next(ui_context(), BUTTON, 0);
        mu_label(ui_context(), "GO");
        mu_end_window(ui_context());
    }
    end_frame();
}

static int
slider_frame(const input_t* in, int* value) {
    ui_begin(in);
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_set_next(ui_context(), BUTTON, 0);
        ui_slider_int(ui_context(), value, 0, 100, 1);
        mu_end_window(ui_context());
    }
    end_frame();
    return *value;
}

static bool
tap(const widget_t* w) {
    const int x = BUTTON.x + BUTTON.w / 2;
    const int y = BUTTON.y + BUTTON.h / 2;
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .point = {x, y}};
    const input_t hold = {.down = true, .point = {x, y}};
    const input_t release = {.released = true, .point = {x, y}};

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
    ui_set_transform(affine2x_identity());
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

typedef struct {
    input_t press, hold, release;
} contact_t;

static contact_t
transformed_contact(int x, int y) {
    const affine2x_t transform = ui_effective_transform();
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){x, y});
    return (contact_t){.press = {.down = true, .pressed = true, .point = mapped_pixel},
                       .hold = {.down = true, .point = mapped_pixel},
                       .release = {.released = true, .point = mapped_pixel}};
}

static void
test_a_reachable_touch_outside_a_button_submits_it(void) {
    const widget_t w = {.enabled = true};
    for (int turn = 0; turn <= 1; turn++) {
        fixture();
        const affine2x_t transform = ui_transform_quarter_turn(turn, GFX_WIDTH, GFX_HEIGHT);
        ui_set_transform(transform);
        const contact_t contact = transformed_contact(BUTTON.x - 20, BUTTON.y + BUTTON.h / 2);
        const input_t idle = {0};

        widget_frame(&w, &idle);
        widget_frame(&w, &idle);
        bool hit = widget_frame(&w, &contact.press);
        for (int i = 0; i < 4; i++) {
            hit |= widget_frame(&w, &contact.hold);
        }
        hit |= widget_frame(&w, &contact.release);
        TEST_ASSERT_TRUE_MESSAGE(hit, turn == 0 ? "portrait reachable touch" : "landscape reachable touch");
    }
}

static void
test_a_reachable_touch_submits_an_unwrapped_microui_button(void) {
    fixture();
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .point = {BUTTON.x - 20, BUTTON.y + BUTTON.h / 2}};
    const input_t hold = {.down = true, .point = {press.point.x, press.point.y}};
    const input_t release = {.released = true, .point = {press.point.x, press.point.y}};

    plain_button_frame(&idle);
    plain_button_frame(&idle);
    bool hit = plain_button_frame(&press);
    for (int i = 0; i < 4; i++) {
        hit |= plain_button_frame(&hold);
    }
    hit |= plain_button_frame(&release);
    TEST_ASSERT_TRUE(hit);
}

static void
test_disabled_controls_and_labels_do_not_aim_a_touch(void) {
    fixture();
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .point = {BUTTON.x - 20, BUTTON.y + BUTTON.h / 2}};
    const widget_t disabled = {.enabled = false};

    widget_frame(&disabled, &idle);
    widget_frame(&disabled, &idle);
    widget_frame(&disabled, &press);
    TEST_ASSERT_EQUAL_INT(press.point.x, ui_context()->mouse_pos.x);

    fixture();
    label_frame(&idle);
    label_frame(&idle);
    label_frame(&press);
    TEST_ASSERT_EQUAL_INT(press.point.x, ui_context()->mouse_pos.x);
}

static void
test_a_slider_drag_uses_raw_moves_after_its_aimed_press(void) {
    fixture();
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .point = {BUTTON.x - 20, BUTTON.y + BUTTON.h / 2}};
    const input_t hold = {.down = true, .point = {press.point.x, press.point.y}};
    const input_t drag = {
        .down = true,
        .point = {BUTTON.x + UI_SLIDER_KNOB_W / 2 + (BUTTON.w - UI_SLIDER_KNOB_W) * 63 / 100, press.point.y}};

    int value = 0;
    value = slider_frame(&idle, &value);
    value = slider_frame(&idle, &value);
    value = slider_frame(&press, &value);
    value = slider_frame(&hold, &value);
    value = slider_frame(&hold, &value);
    value = slider_frame(&drag, &value);
    TEST_ASSERT_EQUAL_INT(63, value);
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
    end_frame();
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
    const int w = draw_font_text_width(draw_font_ui(), "ABC", -1, 2);

    TEST_ASSERT_EQUAL_INT(r.x, text_pos_in(r, "ABC", UI_ALIGN_LEFT).x);
    TEST_ASSERT_EQUAL_INT(r.x + r.w - w, text_pos_in(r, "ABC", UI_ALIGN_RIGHT).x);
    TEST_ASSERT_EQUAL_INT(r.x + (r.w - w) / 2, text_pos_in(r, "ABC", UI_ALIGN_CENTRE).x);
}

static void
test_a_list_goes_below_its_dropdown_when_it_fits(void) {
    const mu_Rect anchor = {20, 100, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 5, 44, GFX_HEIGHT, 16);
    TEST_ASSERT_GREATER_THAN_INT(anchor.y + anchor.h - 1, list.y);
    TEST_ASSERT_EQUAL_INT(5 * 44, list.h);
    TEST_ASSERT_EQUAL_INT(anchor.x, list.x);
    TEST_ASSERT_EQUAL_INT(anchor.w, list.w);
}

static void
test_a_list_grows_upward_from_a_dropdown_near_the_bottom(void) {
    const mu_Rect anchor = {20, 340, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 5, 44, GFX_HEIGHT, 16);
    TEST_ASSERT_LESS_OR_EQUAL_INT(anchor.y, list.y + list.h);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(16, list.y);
}

static bool
overlaps(mu_Rect a, mu_Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void
test_a_list_too_tall_for_either_side_overlays_the_canvas(void) {
    const mu_Rect anchor = {20, 120, 200, 44};
    const mu_Rect list = ui_dropdown_list_rect(anchor, 5, 44, 300, 16);
    TEST_ASSERT_EQUAL_INT(16, list.y);
    TEST_ASSERT_EQUAL_INT(5 * 44, list.h);
    TEST_ASSERT_TRUE(overlaps(list, anchor));
    const mu_Rect landscape = ui_dropdown_list_rect((mu_Rect){20, 120, 200, 62}, 5, 62, 368, 16);
    TEST_ASSERT_EQUAL_INT(5 * 62, landscape.h);
    TEST_ASSERT_EQUAL_INT(0, ui_dropdown_list_scroll(4, 5, 62, landscape.h));
}

/* The exact pixel where ui_dropdown_list_rect() switches sides: a list
 * exactly the room below still fits below, one pixel taller does not and
 * goes above instead, and the same edge holds for the room above, since
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
    TEST_ASSERT_TRUE(overlaps(list, anchor));
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
 * screen: a value only far enough to show the row would satisfy the test
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

static const char* const ITEMS[] = {"ZERO", "ONE", "TWO"};
static const mu_Rect DROPDOWN = {40, 60, 240, 44};

static bool dropdown_open;
static bool paint_frames;

/* One frame of a window holding only the dropdown; returns its pick. */
static int
dropdown_frame(const input_t* in, int selected) {
    ui_begin(in);
    int picked = -1;
    const int opts = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME;
    const bool began = paint_frames ? mu_begin_window_ex(ui_context(), "Widgets", mu_rect(20, 40, 300, 80), opts)
                                    : ui_begin_screen(ui_context(), "Widgets", opts);
    if (began) {
        picked = ui_mu_dropdown(ui_context(), "pick", DROPDOWN, ITEMS, NULL, 3, selected, &THEME);
        dropdown_open = ui_dropdown_is_open(ui_context(), "pick");
        mu_end_window(ui_context());
    }
    if (paint_frames) {
        ui_end(UI_BACKGROUND);
    } else {
        end_frame();
    }
    return picked;
}

static int
dropdown_tap(int x, int y, int selected) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .point = {x, y}};
    const input_t hold = {.down = true, .point = {x, y}};
    const input_t release = {.released = true, .point = {x, y}};
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
    return name_hash64_n(NAME_HASH64_OFFSET_BASIS, p, (size_t)((const unsigned char*)cnt->tail - p));
}

/* The dropdown shows whether its list is open, through its chevron. */
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

/* The chevron hash excludes the button's independently changing bezel. */
static uint64_t
chevron_hash(void) {
    const mu_Rect box = {DROPDOWN.x + DROPDOWN.w - 40, DROPDOWN.y + 8, 32, 28};
    mu_Command* cmd = NULL;
    uint64_t h = NAME_HASH64_OFFSET_BASIS;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type != UI_COMMAND_IMAGE) {
            continue;
        }
        const ui_image_command_t* image = (const ui_image_command_t*)cmd;
        const mu_Rect r = image->rect;
        if (r.x < box.x || r.y < box.y || r.x + r.w > box.x + box.w || r.y + r.h > box.y + box.h) {
            continue;
        }
        h = name_hash64_n(h, cmd, (size_t)cmd->base.size);
    }
    return h;
}

/* was_open is read at the top of ui_mu_dropdown(), before this same frame's own
 * click can open the list, so the very frame the list's own open flag
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
    const input_t press = {.down = true, .pressed = true, .point = {x, y}};
    const input_t hold = {.down = true, .point = {x, y}};

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

/* Closing a list (by a pick, or by a tap outside it) must put the
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

enum {
    MENU_INK = 0xFF00FF,
    MENU_FRAMES = 12,
    MENU_REVISION_FRAMES = 2,
    MENU_DENSE_COMMANDS = 80,
    MENU_OVERFLOW_COMMANDS = UI_BAND_BIN_MAX + 1,
    MENU_ROW_HEIGHT = 62,
    MENU_ITEM_COUNT = 5
};

static const mu_Rect MENU_ANCHOR = {40, 140, 300, MENU_ROW_HEIGHT};
static const char* const MENU_ITEMS[] = {"IDLE", "WALK", "FAST", "GALLOP", "BOUND"};

static int menu_pick;

static void
build_menu_frame(const input_t* in, int revision, int commands, bool scrollable) {
    ui_begin(in);
    mu_Context* ctx = ui_context();
    if (ui_begin_screen(ctx, "Menu", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        menu_pick = ui_mu_dropdown(ctx, "pick", MENU_ANCHOR, MENU_ITEMS, NULL, MENU_ITEM_COUNT, 0, &THEME);
        dropdown_open = ui_dropdown_is_open(ctx, "pick");
        const mu_Color saved_text = ctx->style->colors[MU_COLOR_TEXT];
        ctx->style->colors[MU_COLOR_TEXT] = ui_rgb(MENU_INK);
        mu_layout_set_next(ctx, (mu_Rect){60, 212, 180, 30}, 0);
        mu_label(ctx, revision % 2 ? "CHANGED" : "LABEL");
        mu_Real value = revision;
        mu_layout_set_next(ctx, (mu_Rect){60, 250, 180, 30}, 0);
        mu_slider(ctx, &value, 0, MENU_FRAMES);
        mu_layout_set_next(ctx, (mu_Rect){60, scrollable ? ui_height() : 320, 180, 30}, scrollable);
        mu_label(ctx, "BOOT to close");
        for (int i = 0; i < commands; i++) {
            mu_draw_rect(ctx, (mu_Rect){60 + i, 212 + revision, 1, 100}, ui_rgb(MENU_INK));
        }
        ctx->style->colors[MU_COLOR_TEXT] = saved_text;
        mu_end_window(ctx);
    }
}

static void
test_opening_release_cannot_pick_an_overlay_row(void) {
    const int motion_pixels = 2;
    fixture();
    const input_t idle = {0};
    input_t touch = {
        .down = true, .pressed = true, .point = {MENU_ANCHOR.x + MENU_ANCHOR.w / 2, MENU_ANCHOR.y + MENU_ANCHOR.h / 2}};
    build_menu_frame(&idle, 0, 0, true);
    end_frame();
    build_menu_frame(&idle, 0, 0, true);
    end_frame();
    build_menu_frame(&touch, 0, 0, true);
    end_frame();
    touch.pressed = false;
    for (int frame = 0; frame < UI_DROPDOWN_CLOSE_FRAMES; frame++) {
        touch.point.y += frame < motion_pixels;
        build_menu_frame(&touch, 0, 0, true);
        end_frame();
        TEST_ASSERT_EQUAL_INT(-1, menu_pick);
    }
    touch.down = false;
    touch.released = true;
    build_menu_frame(&touch, 0, 0, true);
    end_frame();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, menu_pick, "opening release must not pick the overlay row under the finger");
    for (int frame = 0; frame < UI_DROPDOWN_CLOSE_FRAMES; frame++) {
        build_menu_frame(&idle, 0, 0, true);
        end_frame();
        TEST_ASSERT_TRUE(dropdown_open);
    }
    const mu_Rect list = ui_dropdown_list_rect(MENU_ANCHOR, MENU_ITEM_COUNT, MENU_ROW_HEIGHT, ui_height(), UI_MARGIN);
    touch = (input_t){.down = true, .pressed = true, .point = {list.x + list.w / 2, list.y + MENU_ROW_HEIGHT / 2}};
    build_menu_frame(&touch, 0, 0, true);
    end_frame();
    int picked = menu_pick;
    touch.pressed = false;
    build_menu_frame(&touch, 0, 0, true);
    end_frame();
    picked = menu_pick >= 0 ? menu_pick : picked;
    touch.down = false;
    touch.released = true;
    build_menu_frame(&touch, 0, 0, true);
    end_frame();
    picked = menu_pick >= 0 ? menu_pick : picked;
    TEST_ASSERT_EQUAL_INT(0, picked);
}

#ifndef DEVICE_BUILD
static uint64_t
framebuffer_rect_hash(mu_Rect r) {
    const gfx_color_t* fb = gfx_framebuffer();
    uint64_t h = NAME_HASH64_OFFSET_BASIS;
    for (int y = r.y; y < r.y + r.h; y++) {
        h = name_hash64_n(h, &fb[y * GFX_WIDTH + r.x], (size_t)r.w * sizeof *fb);
    }
    return h;
}

static const mu_Container* last_menu_canvas;
static bool menu_window_painted;

static void
record_menu_canvas(const mu_Container* canvas) {
    if (canvas == ui_context()->root_list.items[0]) {
        menu_window_painted = true;
    }
    last_menu_canvas = canvas;
}

static void
finish_menu_frame(bool bands) {
    if (bands) {
        ui_end_for_bands(UI_BACKGROUND);
        for (int row = 0; row < GFX_HEIGHT; row += GFX_BAND_HEIGHT) {
            gfx_band_begin_for_test(row);
            ui_replay_band(row, row + GFX_BAND_HEIGHT);
            gfx_band_end_for_test();
        }
    } else {
        ui_end(UI_BACKGROUND);
    }
}

static void
menu_frame(const input_t* in, bool bands, int revision) {
    last_menu_canvas = NULL;
    menu_window_painted = false;
    build_menu_frame(in, revision, MENU_DENSE_COMMANDS, false);
    finish_menu_frame(bands);
}

static void
overflow_frame(bool bands, int revision) {
    const input_t idle = {0};
    ui_begin(&idle);
    mu_Context* ctx = ui_context();
    if (ui_begin_screen(ctx, "Overflow", MU_OPT_NOTITLE | MU_OPT_NOFRAME)) {
        for (int i = 0; i < MENU_OVERFLOW_COMMANDS; i++) {
            mu_draw_rect(ctx, (mu_Rect){BUTTON.x + revision, BUTTON.y, BUTTON.w, BUTTON.h}, ui_rgb(MENU_INK));
        }
        mu_end_window(ctx);
    }
    finish_menu_frame(bands);
}

static uint64_t
owned_command_hash(const mu_Container* canvas) {
    mu_Context* ctx = ui_context();
    uint64_t hash = NAME_HASH64_OFFSET_BASIS;
    mu_Command* cmd = NULL;
    while (mu_next_command(ctx, &cmd)) {
        const mu_Container* owner = NULL;
        for (int i = 0; i < ctx->root_list.idx; i++) {
            const mu_Container* root = ctx->root_list.items[i];
            if ((char*)cmd > (char*)root->head && (char*)cmd < (char*)root->tail
                && (owner == NULL || (char*)root->head > (char*)owner->head)) {
                owner = root;
            }
        }
        if (owner == canvas) {
            hash = name_hash64_n(hash, cmd, (size_t)cmd->base.size);
        }
    }
    return hash;
}

static void
assert_menu_list_on_top(void) {
    const mu_Rect logical =
        ui_dropdown_list_rect(MENU_ANCHOR, MENU_ITEM_COUNT, MENU_ROW_HEIGHT, ui_height(), UI_MARGIN);
    const mu_Rect list = ui_transform_mu_rect(ui_effective_transform(), logical);
    mu_Context* ctx = ui_context();
    TEST_ASSERT_TRUE(dropdown_open);
    TEST_ASSERT_EQUAL_INT(2, ctx->root_list.idx);
    const mu_Container* top = ctx->root_list.items[ctx->root_list.idx - 1];
    TEST_ASSERT_EQUAL_MEMORY(&logical, &top->rect, sizeof logical);
    if (menu_window_painted) {
        TEST_ASSERT_EQUAL_PTR_MESSAGE(top, last_menu_canvas, "the list must repaint after its window");
    }
    const gfx_color_t* fb = gfx_framebuffer();
    for (int y = list.y; y < list.y + list.h; y++) {
        for (int x = list.x; x < list.x + list.w; x++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(RGB565BE(MENU_INK), fb[y * GFX_WIDTH + x],
                                          "menu ink must not bleed through the opaque list");
        }
    }
}

static void
menu_repaint_frames(bool bands) {
    TEST_ASSERT_TRUE(gfx_init());
    fixture();
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
    ui_canvas_painted_for_test = record_menu_canvas;
    draw_clear(RGB565BE(UI_BACKGROUND));
    const contact_t contact = transformed_contact(MENU_ANCHOR.x + 20, MENU_ANCHOR.y + 20);
    const input_t idle = {0};
    menu_frame(&idle, bands, 0);
    menu_frame(&idle, bands, 0);
    const affine2x_t transform = ui_effective_transform();
    const vec2i_t mapped_pixel = affine2x_map_pixel(&transform, (vec2i_t){80, 230});
    TEST_ASSERT_EQUAL_MESSAGE(RGB565BE(MENU_INK), gfx_framebuffer()[mapped_pixel.y * GFX_WIDTH + mapped_pixel.x],
                              "dense menu commands must all be painted before opening the list");
    menu_frame(&contact.press, bands, 0);
    for (int frame = 0; frame < MENU_FRAMES; frame++) {
        const input_t* in = frame < MENU_FRAMES / 2    ? &contact.hold
                            : frame == MENU_FRAMES / 2 ? &contact.release
                                                       : &idle;
        menu_frame(in, bands, frame / 3);
        assert_menu_list_on_top();
        gfx_present();
        if (!bands) {
            const mu_Container* window = ui_context()->root_list.items[0];
            TEST_ASSERT_EQUAL_MESSAGE(owned_command_hash(window), ui_canvas_hash[window - ui_context()->containers],
                                      "a canvas hash must contain only its own drawing commands");
        }
    }
    ui_canvas_painted_for_test = NULL;
    gfx_reset_for_test();
}

static void
test_open_list_stays_on_top_during_held_and_idle_partial_frames(void) {
    menu_repaint_frames(false);
}

static void
test_open_list_stays_on_top_during_held_and_idle_band_frames(void) {
    menu_repaint_frames(true);
}

static void
test_dense_bands_match_a_full_repaint(void) {
    TEST_ASSERT_TRUE(gfx_init());
    fixture();
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
    const input_t idle = {0};
    overflow_frame(false, 0);
    overflow_frame(false, 0);
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    gfx_color_t* expected = memory_alloc(bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(expected);
    overflow_frame(false, 1);
    memcpy(expected, gfx_framebuffer(), bytes);
    draw_clear(RGB565BE(UI_BACKGROUND));
    overflow_frame(true, 1);
    TEST_ASSERT_GREATER_THAN_UINT32(0, ui_band_fallback_count());
    TEST_ASSERT_EQUAL_MEMORY(expected, gfx_framebuffer(), bytes);
    memory_free(expected);
    fixture();
    ui_begin(&idle);
    if (ui_begin_screen(ui_context(), "Sparse", MU_OPT_NOTITLE | MU_OPT_NOFRAME)) {
        mu_draw_rect(ui_context(), BUTTON, ui_rgb(MENU_INK));
        mu_end_window(ui_context());
    }
    ui_end_for_bands(UI_BACKGROUND);
    ui_replay_band(0, GFX_HEIGHT);
    TEST_ASSERT_EQUAL_UINT32(0, ui_band_fallback_count());
    gfx_reset_for_test();
}

/* What an app drew under its UI in band mode: one colour. */
#define UNDER_UI 0x1234

static void
draw_under_ui(int row0, int row1, gfx_color_t* target) {
    for (size_t i = 0; i < (size_t)GFX_WIDTH * (size_t)(row1 - row0); i++) {
        target[i] = UNDER_UI;
    }
}

/* Band mode keeps no framebuffer to paint: plain ui_end() bins the UI, and
 * each band replays it as gfx sends the band. */
static void
test_ui_end_in_band_mode_is_replayed_over_each_band(void) {
    TEST_ASSERT_TRUE(gfx_init());
    fixture();
    const gfx_mode_request_t bands = {.layout = GFX_LAYOUT_BANDS};
    TEST_ASSERT_EQUAL_INT(GFX_LAYOUT_BANDS, gfx_mode_enter(&bands)->layout);
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_PENDING, gfx_readback_begin());
    const input_t idle = {0};
    ui_begin(&idle);
    if (ui_begin_screen(ui_context(), "Banded", MU_OPT_NOTITLE | MU_OPT_NOFRAME)) {
        mu_draw_rect(ui_context(), BUTTON, ui_rgb(MENU_INK));
        mu_end_window(ui_context());
    }
    TEST_ASSERT_TRUE(ui_end(UI_NO_BACKGROUND));
    gfx_band_run(draw_under_ui, ui_replay_band);
    TEST_ASSERT_EQUAL_INT(GFX_READBACK_READY, gfx_readback_begin());
    gfx_color_t row[GFX_WIDTH];
    gfx_read_panel_row(BUTTON.y + (BUTTON.h / 2), row);
    gfx_readback_end();
    gfx_readback_end();
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(MENU_INK), row[BUTTON.x + (BUTTON.w / 2)]);
    TEST_ASSERT_EQUAL_HEX16(UNDER_UI, row[BUTTON.x / 2]);
    gfx_reset_for_test();
}

static void
test_closing_the_list_restores_the_pixels_under_it(void) {
    TEST_ASSERT_TRUE(gfx_init());
    fixture();
    paint_frames = true;
    draw_clear(RGB565BE(UI_BACKGROUND));

    const input_t idle = {0};
    dropdown_frame(&idle, 0);
    dropdown_frame(&idle, 0);
    const mu_Rect list = open_list_rect();
    const uint64_t before = framebuffer_rect_hash(list);

    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(DROPDOWN.x + 20, DROPDOWN.y + 20, 0));
    TEST_ASSERT_TRUE(list_open());
    TEST_ASSERT_NOT_EQUAL(before, framebuffer_rect_hash(list));

    TEST_ASSERT_EQUAL_INT(-1, dropdown_tap(5, ui_height() - 5, 0));
    TEST_ASSERT_FALSE(list_open());
    dropdown_frame(&idle, 0);
    TEST_ASSERT_EQUAL_MESSAGE(before, framebuffer_rect_hash(list), "the vanished list must leave its original pixels");

    paint_frames = false;
    gfx_reset_for_test();
}
#endif

static const char* const MANY[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
#define MANY_COUNT ((int)(sizeof MANY / sizeof MANY[0]))

static bool many_open;

static int
many_frame(const input_t* in, int selected) {
    ui_begin(in);
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        picked = ui_mu_dropdown(ui_context(), "many", DROPDOWN, MANY, NULL, MANY_COUNT, selected, &THEME);
        many_open = ui_dropdown_is_open(ui_context(), "many");
        mu_end_window(ui_context());
    }
    end_frame();
    return picked;
}

static int
many_tap(int x, int y, int selected) {
    const input_t idle = {0};
    const input_t press = {.down = true, .pressed = true, .point = {x, y}};
    const input_t hold = {.down = true, .point = {x, y}};
    const input_t release = {.released = true, .point = {x, y}};
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

/* The "many" dropdown's own open list, found by its rect: it is a nested
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
 * mu_input_scroll(), what ui_bridge_feed() does for a resolved
 * UI_POINTER_SCROLL, without depending on which held frame ui_pointer's own
 * drag threshold happens to trip on. */
static int
many_frame_scroll(int x, int y, int selected, int scroll_dy) {
    const input_t held = {.down = true, .point = {x, y}};
    ui_begin(&held);
    if (scroll_dy != 0) {
        mu_input_scroll(ui_context(), 0, scroll_dy);
    }
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        picked = ui_mu_dropdown(ui_context(), "many", DROPDOWN, MANY, NULL, MANY_COUNT, selected, &THEME);
        many_open = ui_dropdown_is_open(ui_context(), "many");
        mu_end_window(ui_context());
    }
    end_frame();
    return picked;
}

/* A drag-scroll through a tall open list must stick where the finger left
 * it: later frames (held still, or idle once the finger lifts) must not
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
    /* The overlay opens near the bottom scroll limit; drag toward earlier rows. */
    many_frame_scroll(cx, py, MANY_COUNT / 2, -20);

    const int dragged = many_list_scroll(list);
    const int opened_scroll = ui_dropdown_list_scroll(MANY_COUNT / 2, MANY_COUNT, DROPDOWN.h, list.h);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(opened_scroll, dragged, "the drag must actually have moved the scroll");

    many_frame_scroll(cx, py, MANY_COUNT / 2, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(dragged, many_list_scroll(list), "holding still must not move the scroll again");

    many_frame(&(input_t){.released = true, .point = {cx, py}}, MANY_COUNT / 2);
    many_frame(&idle, MANY_COUNT / 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(dragged, many_list_scroll(list),
                                  "a drag-scroll must stick, not snap back to where the list opened");
}

/* One frame of the given full-screen window holding one plain button. */
static bool
named_screen_frame(const char* name, const input_t* in) {
    ui_begin(in);
    bool hit = false;
    if (ui_begin_screen(ui_context(), name, MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_set_next(ui_context(), BUTTON, 0);
        hit = mu_button(ui_context(), "GO") != 0;
        mu_end_window(ui_context());
    }
    end_frame();
    return hit;
}

/* The latency the shell exists to keep: the control answers on the second
 * frame of contact, through the real ui_begin(). */
static void
test_a_tap_submits_on_the_second_frame_of_contact_through_ui_begin(void) {
    fixture();
    const input_t idle = {0};
    plain_button_frame(&idle);
    plain_button_frame(&idle);

    const int x = BUTTON.x + BUTTON.w / 2;
    const int y = BUTTON.y + BUTTON.h / 2;
    TEST_ASSERT_FALSE(plain_button_frame(&(input_t){.down = true, .pressed = true, .point = {x, y}}));
    TEST_ASSERT_TRUE_MESSAGE(plain_button_frame(&(input_t){.down = true, .point = {x, y}}),
                             "the second frame of contact must carry the click");
}

/* A press frame that builds a window microui had not seen the frame before:
 * the seeded root is stale, and the tap must take the extra hover frame
 * rather than vanish. */
static void
test_a_tap_on_the_first_build_of_another_screen_still_submits_once(void) {
    fixture();
    const input_t idle = {0};
    named_screen_frame("Before", &idle);
    named_screen_frame("Before", &idle);

    const int x = BUTTON.x + BUTTON.w / 2;
    const int y = BUTTON.y + BUTTON.h / 2;
    int submits = named_screen_frame("After", &(input_t){.down = true, .pressed = true, .point = {x, y}});
    for (int i = 0; i < 4; i++) {
        submits += named_screen_frame("After", &(input_t){.down = true, .point = {x, y}});
    }
    submits += named_screen_frame("After", &(input_t){.released = true, .point = {x, y}});
    TEST_ASSERT_EQUAL_INT(1, submits);
}

/* A dropdown over a button, the button in the same window. Returns the row
 * picked, and reports the button through *button. */
static int
over_button_frame(const input_t* in, bool* button) {
    ui_begin(in);
    int picked = -1;
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_set_next(ui_context(), BUTTON, 0);
        *button = mu_button(ui_context(), "GO") != 0 || *button;
        picked = ui_mu_dropdown(ui_context(), "pick", DROPDOWN, ITEMS, NULL, 3, 0, &THEME);
        dropdown_open = ui_dropdown_is_open(ui_context(), "pick");
        mu_end_window(ui_context());
    }
    end_frame();
    return picked;
}

typedef struct {
    int picks;
    int buttons;
} taps_t;

static void
over_button_open(void) {
    fixture();
    bool button = false;
    const input_t idle = {0};
    over_button_frame(&idle, &button);
    over_button_frame(&idle, &button);
    const int x = DROPDOWN.x + 20;
    const int y = DROPDOWN.y + 20;
    over_button_frame(&(input_t){.down = true, .pressed = true, .point = {x, y}}, &button);
    for (int i = 0; i < 4; i++) {
        over_button_frame(&(input_t){.down = true, .point = {x, y}}, &button);
    }
    over_button_frame(&(input_t){.released = true, .point = {x, y}}, &button);
    over_button_frame(&idle, &button);
    TEST_ASSERT_TRUE(dropdown_open);
}

static taps_t
over_button_tap(int x, int y, int idle_frames_before) {
    taps_t t = {0};
    bool button = false;
    const input_t idle = {0};
    for (int i = 0; i < idle_frames_before; i++) {
        over_button_frame(&idle, &button);
    }
    int p = over_button_frame(&(input_t){.down = true, .pressed = true, .point = {x, y}}, &button);
    t.picks += p >= 0;
    for (int i = 0; i < 4; i++) {
        p = over_button_frame(&(input_t){.down = true, .point = {x, y}}, &button);
        t.picks += p >= 0;
    }
    p = over_button_frame(&(input_t){.released = true, .point = {x, y}}, &button);
    t.picks += p >= 0;
    t.buttons = button ? 1 : 0;
    return t;
}

/* The list of a pick closes on a frame counted from the pick; a press that
 * lands on any of those frames must reach exactly one thing: the row while
 * the list is up, the button beneath once it is gone, never neither. */
static void
test_a_tap_on_a_closing_list_reaches_exactly_one_thing(void) {
    for (int wait = 0; wait <= UI_DROPDOWN_CLOSE_FRAMES + 1; wait++) {
        over_button_open();
        const mu_Rect list = open_list_rect();
        const int x = list.x + 20;
        const int y = BUTTON.y + BUTTON.h / 2;
        const taps_t first = over_button_tap(x, y, 0);
        TEST_ASSERT_EQUAL_INT(1, first.picks);
        const taps_t t = over_button_tap(x, y, wait);
        char why[64];
        snprintf(why, sizeof why, "wait %d picks %d buttons %d", wait, t.picks, t.buttons);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.picks + t.buttons, why);
    }
}

static void
test_a_one_frame_tap_on_an_open_list_row_picks_it_and_closes_it(void) {
    over_button_open();
    const mu_Rect list = open_list_rect();
    bool button = false;
    const input_t idle = {0};
    const int picked = over_button_frame(
        &(input_t){.pressed = true, .released = true, .point = {list.x + 20, list.y + DROPDOWN.h + 20}}, &button);
    TEST_ASSERT_EQUAL_INT(1, picked);
    TEST_ASSERT_FALSE_MESSAGE(button, "the button beneath the list must not be clicked through it");
    for (int i = 0; i < UI_DROPDOWN_CLOSE_FRAMES + 1; i++) {
        over_button_frame(&idle, &button);
    }
    TEST_ASSERT_FALSE(dropdown_open);
}

static void
test_a_one_frame_tap_outside_an_open_list_closes_it_and_picks_nothing(void) {
    over_button_open();
    bool button = false;
    const input_t idle = {0};
    const int picked =
        over_button_frame(&(input_t){.pressed = true, .released = true, .point = {5, ui_height() - 5}}, &button);
    TEST_ASSERT_EQUAL_INT(-1, picked);
    TEST_ASSERT_FALSE(button);
    over_button_frame(&idle, &button);
    TEST_ASSERT_FALSE(dropdown_open);
}

/* The lift went to frames that never reached the UI; the next press proves
 * it, and must still get a control hovered and clicked. */
static void
test_a_press_after_an_unseen_lift_still_submits(void) {
    fixture();
    const input_t idle = {0};
    plain_button_frame(&idle);
    plain_button_frame(&idle);

    /* The first press lands on empty space, so nothing is left hovered for
     * the second to fall back on. */
    plain_button_frame(&(input_t){.down = true, .pressed = true, .point = {10, ui_height() - 10}});
    plain_button_frame(&(input_t){.down = true, .point = {10, ui_height() - 10}});

    const int x = BUTTON.x + BUTTON.w / 2;
    const int y = BUTTON.y + BUTTON.h / 2;
    int submits = plain_button_frame(&(input_t){.down = true, .pressed = true, .point = {x, y}});
    submits += plain_button_frame(&(input_t){.down = true, .point = {x, y}});
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, submits, "the second press must land although the first lift was never seen");
}

/* A finger resting on a button before ui_pointer lands its press (which
 * is all a drag across a list ever is) must not draw it pressed; the
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
    const input_t press = {.down = true, .pressed = true, .point = {x, y}};
    const input_t hold = {.down = true, .point = {x, y}};
    widget_frame(&w, &press);
    TEST_ASSERT_EQUAL_MESSAGE(at_rest, canvas_hash("Widgets"), "hovered but not yet pressed must look at rest");

    widget_frame(&w, &hold);
    widget_frame(&w, &hold);
    const uint64_t pressed = canvas_hash("Widgets");

    const input_t release = {.released = true, .point = {x, y}};
    widget_frame(&w, &release);
    widget_frame(&w, &idle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(at_rest, pressed, "a landed press must look pressed");
}

/* A press that lands off the button and only slides onto it afterward is
 * exactly what a drag across a list looks like to a plain button beneath
 * it; it must never draw pressed and never report a click, since the
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
    const int off_x = BUTTON.x - 60;

    const input_t press_off = {.down = true, .pressed = true, .point = {off_x, by}};
    const input_t hold_off = {.down = true, .point = {off_x, by}};
    const input_t hold_on = {.down = true, .point = {bx, by}};
    const input_t release_on = {.released = true, .point = {bx, by}};

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
    end_frame();
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
swatch_frame(mu_Rect r, const uint32_t* colors, int cols, int rows) {
    const input_t idle = {0};
    ui_begin(&idle);
    if (ui_begin_screen(ui_context(), "Widgets", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        ui_swatch_grid(ui_context(), r, colors, cols, rows);
        mu_end_window(ui_context());
    }
    end_frame();
}

static void
test_ui_swatch_grid_is_one_command_with_owned_colors(void) {
    fixture();
    const mu_Rect r = {10, 20, 100, 33};
    const int cols = 3;
    const int rows = 2;
    uint32_t colors[6];
    for (int i = 0; i < 6; i++) {
        colors[i] = (uint32_t)(0x010203 * (i + 1));
    }
    swatch_frame(r, colors, cols, rows);

    int n = 0;
    mu_Command* cmd = NULL;
    while (mu_next_command(ui_context(), &cmd)) {
        if (cmd->type == UI_COMMAND_SWATCHES) {
            const ui_swatch_command_t* grid = (const ui_swatch_command_t*)cmd;
            TEST_ASSERT_EQUAL_INT(cols, grid->cols);
            TEST_ASSERT_EQUAL_INT(rows, grid->rows);
            TEST_ASSERT_EQUAL_MEMORY(colors, grid->rgb, sizeof colors);
            colors[0]++;
            TEST_ASSERT_NOT_EQUAL(colors[0], grid->rgb[0]);
            n++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, n);
}

static void
test_icon_is_one_command_and_obeys_clip(void) {
    fixture();
    static const uint8_t bits[] = {0xA0, 0x40};
    const image_t image = {
        .pixels = (void*)bits, .format = IMAGE_MONO1, .width = 3, .height = 2, .pitch = 8 / CHAR_BIT};
    mu_Context* ctx = ui_context();
    const mu_Rect box = {10, 20, 30, 20};
    ctx->clip_stack.items[ctx->clip_stack.idx++] = box;
    ctx->command_list.idx = 0;
    ui_draw_icon(ctx, box, &image, NULL, THEME.text);
    mu_Command* cmd = NULL;
    TEST_ASSERT_TRUE(mu_next_command(ctx, &cmd));
    TEST_ASSERT_EQUAL_INT(UI_COMMAND_IMAGE, cmd->type);
    TEST_ASSERT_FALSE(mu_next_command(ctx, &cmd));
    ctx->command_list.idx = 0;
    ui_draw_icon(ctx, (mu_Rect){0, 20, 30, 20}, &image, NULL, THEME.text);
    cmd = NULL;
    TEST_ASSERT_TRUE(mu_next_command(ctx, &cmd));
    TEST_ASSERT_EQUAL_INT(MU_COMMAND_CLIP, cmd->type);
    TEST_ASSERT_TRUE(mu_next_command(ctx, &cmd));
    TEST_ASSERT_EQUAL_INT(UI_COMMAND_IMAGE, cmd->type);
    TEST_ASSERT_TRUE(mu_next_command(ctx, &cmd));
    TEST_ASSERT_EQUAL_INT(MU_COMMAND_CLIP, cmd->type);
    TEST_ASSERT_FALSE(mu_next_command(ctx, &cmd));
    ctx->command_list.idx = 0;
    ui_draw_icon(ctx, (mu_Rect){100, 100, 30, 20}, &image, NULL, THEME.text);
    TEST_ASSERT_EQUAL_INT(0, ctx->command_list.idx);
    mu_pop_clip_rect(ctx);
}

static const uint8_t COMMAND_ICON_BITS[] = {0xA0, 0x40};
static const image_t COMMAND_ICON = {
    .pixels = (void*)COMMAND_ICON_BITS, .format = IMAGE_MONO1, .width = 3, .height = 2, .pitch = 8 / CHAR_BIT};
static const mu_Rect COMMAND_BOX = {10, 20, 31, 23};
#define COMMAND_COLS 3
#define COMMAND_ROWS 2
static const uint32_t COMMAND_COLORS[COMMAND_COLS * COMMAND_ROWS] = {0x123456, 0xABCDEF, 0x102030,
                                                                     0xFFEEDD, 0x010203, 0xFEDCBA};

static void
reference_icon_rect(void* context, recti_t r) {
    mu_draw_rect(context, (mu_Rect){COMMAND_BOX.x + r.x, COMMAND_BOX.y + r.y, r.w, r.h}, THEME.text);
}

/* What the builders under test draw: one command each. */
static void
draw_command_subject(mu_Context* ctx, bool swatches) {
    if (swatches) {
        uint32_t colors[COMMAND_COLS * COMMAND_ROWS];
        memcpy(colors, COMMAND_COLORS, sizeof colors);
        ui_swatch_grid(ctx, COMMAND_BOX, colors, COMMAND_COLS, COMMAND_ROWS);
        memset(colors, 0, sizeof colors);
    } else {
        ui_draw_icon(ctx, COMMAND_BOX, &COMMAND_ICON, NULL, THEME.text);
    }
}

/* The same picture as one rect per cell or run, as the builders drew it before. */
static void
draw_command_reference(mu_Context* ctx, bool swatches) {
    if (!swatches) {
        icon_walk_blocks(&COMMAND_ICON, COMMAND_BOX.w, COMMAND_BOX.h, reference_icon_rect, ctx);
        return;
    }
    for (int row = 0; row < COMMAND_ROWS; row++) {
        for (int col = 0; col < COMMAND_COLS; col++) {
            const int x = COMMAND_BOX.x + col * COMMAND_BOX.w / COMMAND_COLS;
            const int y = COMMAND_BOX.y + row * COMMAND_BOX.h / COMMAND_ROWS;
            const int right = COMMAND_BOX.x + (col + 1) * COMMAND_BOX.w / COMMAND_COLS;
            const int bottom = COMMAND_BOX.y + (row + 1) * COMMAND_BOX.h / COMMAND_ROWS;
            mu_draw_rect(ctx, (mu_Rect){x, y, right - x, bottom - y}, ui_rgb(COMMAND_COLORS[row * COMMAND_COLS + col]));
        }
    }
}

static void
paint_command_reference(bool reference, bool swatches, bool clipped) {
    ui_invalidate();
    ui_begin(NULL);
    mu_Context* ctx = ui_context();
    if (ui_begin_screen(ctx, "Command pixels", MU_OPT_NOTITLE | MU_OPT_NOFRAME)) {
        if (clipped) {
            mu_push_clip_rect(ctx, (mu_Rect){COMMAND_BOX.x + 7, COMMAND_BOX.y + 5, 17, 13});
        }
        if (reference) {
            draw_command_reference(ctx, swatches);
        } else {
            draw_command_subject(ctx, swatches);
        }
        if (clipped) {
            mu_pop_clip_rect(ctx);
        }
        mu_end_window(ctx);
    }
    ui_end(UI_BACKGROUND);
}

static void
test_image_and_swatch_commands_match_rect_pixels(void) {
#ifndef DEVICE_BUILD
    TEST_ASSERT_TRUE(gfx_init());
#endif
    fixture();
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    gfx_color_t* expected = memory_alloc(bytes, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(expected);
    for (int turn = 0; turn < 4; turn++) {
        ui_set_transform(ui_transform_quarter_turn(turn, GFX_WIDTH, GFX_HEIGHT));
        for (int swatches = 0; swatches < 2; swatches++) {
            for (int clipped = 0; clipped < 2; clipped++) {
                paint_command_reference(true, swatches, clipped);
                memcpy(expected, gfx_framebuffer(), bytes);
                paint_command_reference(false, swatches, clipped);
                const bool equal = memcmp(expected, gfx_framebuffer(), bytes) == 0;
                if (!equal) {
                    memory_free(expected);
#ifndef DEVICE_BUILD
                    gfx_reset_for_test();
#endif
                    TEST_FAIL_MESSAGE("custom command pixels differ from rectangle reference");
                }
            }
        }
    }
    memory_free(expected);
    fixture();
#ifndef DEVICE_BUILD
    gfx_reset_for_test();
#endif
}

static void test_menu_band_frame_cost(void);
static void test_options_band_frame_cost(void);

void
run_ui_widgets_suite(void) {
    RUN_TEST(test_an_enabled_button_reports_a_tap);
    RUN_TEST(test_a_disabled_button_takes_no_tap);
    RUN_TEST(test_a_reachable_touch_outside_a_button_submits_it);
    RUN_TEST(test_a_reachable_touch_submits_an_unwrapped_microui_button);
    RUN_TEST(test_disabled_controls_and_labels_do_not_aim_a_touch);
    RUN_TEST(test_a_slider_drag_uses_raw_moves_after_its_aimed_press);
    RUN_TEST(test_an_icon_leaves_less_room_for_the_label);
    RUN_TEST(test_text_aligns_to_either_edge_or_the_centre);
    RUN_TEST(test_opening_release_cannot_pick_an_overlay_row);
    RUN_TEST(test_a_list_goes_below_its_dropdown_when_it_fits);
    RUN_TEST(test_a_list_grows_upward_from_a_dropdown_near_the_bottom);
    RUN_TEST(test_a_list_too_tall_for_either_side_overlays_the_canvas);
    RUN_TEST(test_a_list_switches_sides_at_the_exact_pixel_of_room);
    RUN_TEST(test_a_tap_on_a_listed_item_picks_it_and_closes_the_list);
    RUN_TEST(test_the_list_closes_within_the_close_frame_budget);
    RUN_TEST(test_a_tap_elsewhere_closes_the_list_without_a_pick);
    RUN_TEST(test_opening_the_list_changes_the_screen_under_it);
    RUN_TEST(test_the_chevron_lags_the_lists_own_open_flag_by_one_frame);
    RUN_TEST(test_closing_the_list_restores_the_closed_drawing);
#ifndef DEVICE_BUILD
    RUN_TEST(test_menu_band_frame_cost);
    RUN_TEST(test_options_band_frame_cost);
    RUN_TEST(test_dense_bands_match_a_full_repaint);
    RUN_TEST(test_closing_the_list_restores_the_pixels_under_it);
    RUN_TEST(test_ui_end_in_band_mode_is_replayed_over_each_band);
    RUN_TEST(test_open_list_stays_on_top_during_held_and_idle_partial_frames);
    RUN_TEST(test_open_list_stays_on_top_during_held_and_idle_band_frames);
#endif
    RUN_TEST(test_a_list_that_fits_does_not_scroll);
    RUN_TEST(test_only_a_real_press_draws_a_button_pressed);
    RUN_TEST(test_a_press_that_slides_onto_a_button_from_off_it_is_not_a_click);
    RUN_TEST(test_a_tap_submits_on_the_second_frame_of_contact_through_ui_begin);
    RUN_TEST(test_a_tap_on_the_first_build_of_another_screen_still_submits_once);
    RUN_TEST(test_a_press_after_an_unseen_lift_still_submits);
    RUN_TEST(test_a_tap_on_a_closing_list_reaches_exactly_one_thing);
    RUN_TEST(test_a_one_frame_tap_on_an_open_list_row_picks_it_and_closes_it);
    RUN_TEST(test_a_one_frame_tap_outside_an_open_list_closes_it_and_picks_nothing);
    RUN_TEST(test_a_list_its_dropdown_stopped_drawing_is_closed_when_it_returns);
    RUN_TEST(test_a_list_taller_than_the_screen_stays_inside_it);
    RUN_TEST(test_a_list_opens_scrolled_to_its_current_item);
    RUN_TEST(test_a_list_opens_scrolled_so_the_row_sits_centred);
    RUN_TEST(test_the_last_row_of_a_list_taller_than_the_screen_can_be_picked);
    RUN_TEST(test_a_drag_scroll_sticks_through_later_frames);
    RUN_TEST(test_ui_swatch_grid_is_one_command_with_owned_colors);
    RUN_TEST(test_icon_is_one_command_and_obeys_clip);
    RUN_TEST(test_image_and_swatch_commands_match_rect_pixels);
    RUN_TEST(test_theme_from_style_maps_widget_colors);
}

SUITE_REGISTER(run_ui_widgets_suite);
SUITE_READS(run_ui_widgets_suite, SAND_PACK);

static void
paint_menu_band(int row0, int row1, gfx_color_t* target) {
    (void)target;
    draw_rect(0, row0, GFX_WIDTH, row1 - row0, RGB565BE(UI_BACKGROUND));
    ui_replay_band(row0, row1);
}

static const gfx_mode_request_t BAND_MODE_REQUEST = {.layout = GFX_LAYOUT_BANDS};

static bool band_icons_owned;
static sand_mode_swatch_t* band_swatches;

typedef struct {
    int32_t frame_us[MENU_FRAMES];
    uint32_t fallbacks[MENU_FRAMES];
} band_samples_t;

static band_samples_t* band_samples;

enum { BAND_OPTIONS_WARMUP_FRAMES = 5 };

typedef struct {
    sand_menu_t menu;
    options_screen_labels_t labels;
    options_screen_layout_t layout;
    contact_t contact;
    input_t idle;
    const input_t* warmup[BAND_OPTIONS_WARMUP_FRAMES];
} band_options_fixture_t;

static band_options_fixture_t* band_options_fixture;

static void
restore_menu_band_fixture(void) {
    if (band_icons_owned) {
        ui_icon_set_release(&sand_icon_set);
        band_icons_owned = false;
    }
    memory_free(band_samples);
    band_samples = NULL;
    memory_free(band_options_fixture);
    band_options_fixture = NULL;
    memory_free(band_swatches);
    band_swatches = NULL;
    ui_clear_band_overlay();
    gfx_mode_exit();
    fixture();
#ifndef DEVICE_BUILD
    gfx_reset_for_test();
#endif
}

static void
report_band_case(const char* name, band_samples_t* samples) {
    uint32_t total_fallbacks = 0;
    for (int frame = 0; frame < MENU_FRAMES; frame++) {
        total_fallbacks += samples->fallbacks[frame];
    }
    const bench_stats_t stats = bench_stats_compute(samples->frame_us, MENU_FRAMES);
#ifdef DEVICE_BUILD
    ESP_LOGI("ui_band_perf", "case=%s frames=%d fallbacks=%lu frame_us=%lld max_us=%lld", name, MENU_FRAMES,
             (unsigned long)total_fallbacks, (long long)stats.avg, (long long)stats.max);
#else
    (void)name;
    (void)total_fallbacks;
    (void)stats;
#endif
}

static void
sample_band_frame(int frame, int64_t began_us) {
    band_samples->frame_us[frame] = (int32_t)(timing_now_us() - began_us);
    band_samples->fallbacks[frame] = ui_band_fallback_count();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, band_samples->fallbacks[frame], "product cases must fit the band bin");
}

static void
allocate_band_samples(void) {
    band_samples = memory_alloc(sizeof *band_samples, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(band_samples);
    memset(band_samples, 0, sizeof *band_samples);
}

static void
test_menu_band_frame_cost(void) {
#ifndef DEVICE_BUILD
    TEST_ASSERT_TRUE(gfx_init());
    const unsigned guard_trips = gfx_fb_guard_trips_for_test();
#endif
    suite_set_test_cleanup(restore_menu_band_fixture);
    TEST_ASSERT_EQUAL(GFX_LAYOUT_BANDS, gfx_mode_enter(&BAND_MODE_REQUEST)->layout);
    for (int dense = 0; dense <= 1; dense++) {
        fixture();
        ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
        const contact_t contact = transformed_contact(MENU_ANCHOR.x + 20, MENU_ANCHOR.y + 20);
        const input_t idle = {0};
        const input_t* warmup[] = {&idle, &idle, &contact.press, &contact.hold};
        const int commands = dense ? MENU_DENSE_COMMANDS : 0;
        allocate_band_samples();
        for (size_t i = 0; i < sizeof warmup / sizeof warmup[0]; i++) {
            gfx_present_begin();
            gfx_present_wait();
            ui_clear_band_overlay();
            build_menu_frame(warmup[i], 0, commands, false);
            ui_end_for_bands(UI_BACKGROUND);
            gfx_invalidate();
            gfx_band_run(paint_menu_band, NULL);
        }
        TEST_ASSERT_TRUE(dropdown_open);
        for (int frame = 0; frame < MENU_FRAMES; frame++) {
            gfx_present_begin();
            gfx_present_wait();
            ui_clear_band_overlay();
            const int64_t began_us = timing_now_us();
            build_menu_frame(&idle, frame / MENU_REVISION_FRAMES, commands, false);
            ui_end_for_bands(UI_BACKGROUND);
            gfx_invalidate();
            gfx_band_run(paint_menu_band, NULL);
            sample_band_frame(frame, began_us);
        }
        report_band_case(dense ? "dense_menu" : "sparse_menu", band_samples);
        memory_free(band_samples);
        band_samples = NULL;
    }
#ifndef DEVICE_BUILD
    TEST_ASSERT_EQUAL_UINT(guard_trips, gfx_fb_guard_trips_for_test());
#endif
    restore_menu_band_fixture();
    suite_clear_test_cleanup();
}

void
run_ui_band_perf_suite(void) {
    RUN_TEST(test_menu_band_frame_cost);
    RUN_TEST(test_options_band_frame_cost);
}

SUITE_REGISTER_ON_REQUEST(run_ui_band_perf_suite);
SUITE_READS(run_ui_band_perf_suite, SAND_PACK);

#define BAND_QUALITY_NORMAL 2

static const char* const BAND_QUALITY_NAMES[] = {"ULTRA", "HIGH", "NORMAL", "LOW", "VERY LOW"};
static const char* const BAND_DITHER_NAMES[GFX_DITHER_MODE_COUNT] = {
    "NONE", "CELL CHECKER", "CELL BAYER2", "PIXEL CHECKER2", "PIXEL BAYER4",
};
static const sand_options_t BAND_OPTIONS = {
    .quality = BAND_QUALITY_NORMAL,
    .color = SAND_COLOUR_16,
    .dither = GFX_DITHER_CELL_BAYER2,
};

static void
paint_options_band(int row0, int row1, gfx_color_t* target) {
    (void)target;
    draw_rect(0, row0, GFX_WIDTH, row1 - row0, RGB565BE(UI_BACKGROUND));
    ui_replay_band(row0, row1);
}

static void
options_band_frame(const input_t* input, const sand_menu_t* menu, const options_screen_labels_t* labels) {
    ui_begin(input);
    options_screen_draw(menu, BAND_OPTIONS, labels);
    ui_end_for_bands(UI_BACKGROUND);
    gfx_invalidate();
    gfx_band_run(paint_options_band, NULL);
}

/* Setup temporaries must leave the stack before the drawing call chain. */
static __attribute__((noinline)) void
prepare_options_band_case(bool open) {
    fixture();
    ui_set_transform(ui_transform_quarter_turn(0, GFX_WIDTH, GFX_HEIGHT));
    band_options_fixture_t* const f = band_options_fixture;
    options_screen_layout(ui_width(), ui_height(), &f->layout);
    f->contact =
        transformed_contact(f->layout.dither.x + f->layout.dither.w / 2, f->layout.dither.y + f->layout.dither.h / 2);
    f->warmup[0] = &f->idle;
    f->warmup[1] = &f->idle;
    f->warmup[2] = open ? &f->contact.press : &f->idle;
    f->warmup[3] = open ? &f->contact.hold : &f->idle;
    f->warmup[4] = open ? &f->contact.release : &f->idle;
}

static void
measure_options_band_case(bool open, const char* name) {
    prepare_options_band_case(open);
    band_options_fixture_t* const f = band_options_fixture;
    allocate_band_samples();
    for (size_t i = 0; i < BAND_OPTIONS_WARMUP_FRAMES; i++) {
        gfx_present_begin();
        gfx_present_wait();
        ui_clear_band_overlay();
        options_band_frame(f->warmup[i], &f->menu, &f->labels);
    }
    TEST_ASSERT_EQUAL_INT(open ? 2 : 1, ui_context()->root_list.idx);
    for (int frame = 0; frame < MENU_FRAMES; frame++) {
        gfx_present_begin();
        gfx_present_wait();
        ui_clear_band_overlay();
        const int64_t began_us = timing_now_us();
        options_band_frame(&f->idle, &f->menu, &f->labels);
        sample_band_frame(frame, began_us);
    }
    report_band_case(name, band_samples);
    memory_free(band_samples);
    band_samples = NULL;
}

static void
test_options_band_frame_cost(void) {
#ifndef DEVICE_BUILD
    TEST_ASSERT_TRUE(gfx_init());
#endif
    suite_set_test_cleanup(restore_menu_band_fixture);
    band_icons_owned = !sand_icon_set.loaded;
    TEST_ASSERT_TRUE(ui_icon_set_load(&sand_icon_set));
    TEST_ASSERT_EQUAL(GFX_LAYOUT_BANDS, gfx_mode_enter(&BAND_MODE_REQUEST)->layout);
    band_swatches = memory_alloc(SAND_COLOUR_MODE_COUNT * sizeof *band_swatches, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(band_swatches);
    sand_mode_swatches(sand_dither_none_lut, sand_palette256_lut, GFX_INDEXED_PALETTE_SIZE, SAND_PALETTE_UI_ENTRIES,
                       band_swatches);
    band_options_fixture = memory_alloc(sizeof *band_options_fixture, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(band_options_fixture);
    memset(band_options_fixture, 0, sizeof *band_options_fixture);
    band_options_fixture->menu = (sand_menu_t){.screen = SAND_MENU_OPTIONS, .draft = BAND_OPTIONS};
    band_options_fixture->labels = (options_screen_labels_t){
        .quality_names = BAND_QUALITY_NAMES,
        .quality_count = sizeof BAND_QUALITY_NAMES / sizeof BAND_QUALITY_NAMES[0],
        .dither_names = BAND_DITHER_NAMES,
        .dither_count = GFX_DITHER_MODE_COUNT,
        .mode_swatches = band_swatches,
    };
    measure_options_band_case(false, "sand_options_closed");
    measure_options_band_case(true, "sand_options_dither_open");
    restore_menu_band_fixture();
    suite_clear_test_cleanup();
}
