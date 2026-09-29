/*
 * Portable suite: ui_pointer driving REAL microui.
 *
 * suite_ui_pointer.c only asserts the event LIST ui_pointer_step()
 * produces - a list of events matching does not mean microui can resolve
 * a click from them; only microui itself decides that. This suite links
 * real microui.c to prove it.
 *
 * The mechanism, so nobody re-derives it from scratch: mu_mouse_over() needs
 * in_hover_root(), and mu_begin() copies hover_root from the PREVIOUS frame's
 * next_hover_root. mu_update_control() then marks a control hovered only when
 * the mouse is over it AND mouse_down is clear, and focus is only taken from
 * a control that is already hovered. So a DOWN fed before hover has settled
 * lands with nothing hovered, nothing takes focus, and mu_button() renders a
 * pressed frame while returning 0 forever.
 *
 * microui.c is plain C over stdio/stdlib/string, so it links here unchanged -
 * this is the only suite that links it.
 */

#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "microui.h"
#include "ui/ui_bridge.h"
#include "ui/ui_pointer.h"

#define CANVAS_W 368
#define CANVAS_H 448

/* A button big enough that no rounding puts the touch point outside it. */
#define BTN_X    40
#define BTN_Y    80
#define BTN_W    280
#define BTN_H    64

/* Heap, not a file-scope object: a mu_Context is 10,744 bytes, and the
 * diagnostics build links every suite into firmware, where internal heap
 * headroom is scarce enough that a second context in .bss would not be
 * free - something host tests, with a laptop's memory behind them,
 * cannot notice. Allocated for the suite's run, outside any one test, and
 * reset by every test's fixture(). */
static mu_Context* ctx;
static ui_pointer_t pointer;
static mu_Id button_id;

/* microui measures text through the context; the real shell hands it a font
 * atlas, and nothing here cares how wide a glyph is. */
static int
stub_text_width(mu_Font font, const char* str, int len) {
    (void)font;
    return (len < 0 ? (int)strlen(str) : len) * 8;
}

static int
stub_text_height(mu_Font font) {
    (void)font;
    return 8;
}

static void
fixture(void) {
    TEST_ASSERT_NOT_NULL(ctx);
    memset(ctx, 0, sizeof *ctx);
    memset(&pointer, 0, sizeof pointer);
    mu_init(ctx);
    ctx->text_width = stub_text_width;
    ctx->text_height = stub_text_height;
}

/* One frame of the real bridge: translate input_t exactly as ui.c's
 * feed_input() does, then build a full-screen window holding one button.
 * Returns whether the button submitted this frame. */
static bool
frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    in.x = x;
    in.y = y;

    ui_bridge_feed(ctx, &pointer, &in);

    bool submitted = false;
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, "screen", mu_rect(0, 0, CANVAS_W, CANVAS_H),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_layout_set_next(ctx, mu_rect(BTN_X, BTN_Y, BTN_W, BTN_H), 0);
        if (mu_button(ctx, "GO")) {
            submitted = true;
        }
        button_id = ctx->last_id;
        mu_end_window(ctx);
    }
    mu_end(ctx);
    ui_bridge_end(ctx, &pointer);
    return submitted;
}

/* Frames with nothing touching, so the pointer parks off-screen exactly as
 * it does whenever a finger is not on the glass. */
static void
idle_frames(int count) {
    for (int i = 0; i < count; i++) {
        frame(false, false, false, 0, 0);
    }
}

/* A tap as touch_fsm actually delivers one: a pressed edge, some frames of
 * being held, then a released edge. */
static int
taps_counted(int held_frames) {
    const int cx = BTN_X + BTN_W / 2;
    const int cy = BTN_Y + BTN_H / 2;
    int submits = 0;

    if (frame(true, true, false, cx, cy)) {
        submits++;
    }
    for (int i = 0; i < held_frames; i++) {
        if (frame(true, false, false, cx, cy)) {
            submits++;
        }
    }
    if (frame(false, false, true, cx, cy)) {
        submits++;
    }
    return submits;
}

/* The regression this suite was written for. */

static void
test_a_tap_submits_the_button_underneath_it(void) {
    fixture();
    idle_frames(2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, taps_counted(4),
                                  "a tap must submit the button under it exactly once - this is the "
                                  "assertion a held-DOWN policy broke while every event-list test "
                                  "stayed green, leaving no app reachable from the launcher");
}

/* Latency: the press is answered on the second frame of contact, not the
 * third. The DOWN is fed one frame after the first position, so microui must
 * already have granted hover on that first frame. */
static void
test_a_tap_submits_on_the_second_frame_of_contact(void) {
    fixture();
    idle_frames(2);

    const int cx = BTN_X + BTN_W / 2;
    const int cy = BTN_Y + BTN_H / 2;
    TEST_ASSERT_FALSE(frame(true, true, false, cx, cy));
    TEST_ASSERT_TRUE_MESSAGE(frame(true, false, false, cx, cy),
                             "the DOWN follows the first frame of contact, and it must click");
    TEST_ASSERT_FALSE_MESSAGE(frame(true, false, false, cx, cy), "holding must not click again");
    TEST_ASSERT_FALSE(frame(false, false, true, cx, cy));
}

/* Hover is resolved on the frame the finger lands, and on the control under
 * it - not the one after, and not another. On empty space inside a root the
 * root is hovered and no control is. */
static void
test_hover_is_granted_on_the_frame_a_press_lands(void) {
    fixture();
    idle_frames(2);

    frame(true, true, false, BTN_X + BTN_W / 2, BTN_Y + BTN_H / 2);
    TEST_ASSERT_NOT_NULL(ctx->hover_root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(button_id, ctx->hover, "the button under the finger is the one hovered");

    fixture();
    idle_frames(2);
    frame(true, true, false, BTN_X / 2, CANVAS_H - 4);
    TEST_ASSERT_NOT_NULL_MESSAGE(ctx->hover_root, "the window under the finger is the hover root");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, ctx->hover, "empty space hovers no control");
}

/* Two windows over the press point: the seed must pick the one on top, as
 * begin_root_container() does, or the wrong window's button would be hovered
 * and click through. Returns 1 for the back button, 2 for the front one. */
static int
stacked_frame(const input_t* in) {
    ui_bridge_feed(ctx, &pointer, in);

    int submitted = 0;
    const int opt = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME;
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, "back", mu_rect(0, 0, CANVAS_W, CANVAS_H), opt)) {
        mu_layout_set_next(ctx, mu_rect(BTN_X, BTN_Y, BTN_W, BTN_H), 0);
        submitted += mu_button(ctx, "BACK") ? 1 : 0;
        mu_end_window(ctx);
    }
    if (mu_begin_window_ex(ctx, "front", mu_rect(BTN_X, BTN_Y, BTN_W, BTN_H), opt)) {
        mu_layout_set_next(ctx, mu_rect(BTN_X, BTN_Y, BTN_W, BTN_H), 0);
        submitted += mu_button(ctx, "FRONT") ? 2 : 0;
        mu_end_window(ctx);
    }
    mu_end(ctx);
    ui_bridge_end(ctx, &pointer);
    return submitted;
}

static void
test_a_press_over_stacked_windows_reaches_only_the_top_one(void) {
    fixture();
    const input_t idle = {0};
    stacked_frame(&idle);
    stacked_frame(&idle);

    const int x = BTN_X + BTN_W / 2;
    const int y = BTN_Y + BTN_H / 2;
    int total = stacked_frame(&(input_t){.down = true, .pressed = true, .x = x, .y = y});
    total += stacked_frame(&(input_t){.down = true, .x = x, .y = y});
    total += stacked_frame(&(input_t){.down = true, .x = x, .y = y});
    total += stacked_frame(&(input_t){.released = true, .x = x, .y = y});
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, total, "only the window on top may take the press, once");
}

/* Two windows sharing an edge, the one that would win a tie built last: a
 * point on the shared line belongs to the window whose rect starts there. */
static void
edge_frame(const input_t* in, const char* first, mu_Rect first_rect, const char* second, mu_Rect second_rect) {
    ui_bridge_feed(ctx, &pointer, in);
    const int opt = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME;
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, first, first_rect, opt)) {
        mu_end_window(ctx);
    }
    if (mu_begin_window_ex(ctx, second, second_rect, opt)) {
        mu_end_window(ctx);
    }
    mu_end(ctx);
    ui_bridge_end(ctx, &pointer);
}

static void
assert_seed_lands_in(int x, int y, const char* want, const char* first, mu_Rect first_rect, const char* second,
                     mu_Rect second_rect) {
    fixture();
    const input_t idle = {0};
    edge_frame(&idle, first, first_rect, second, second_rect);
    edge_frame(&idle, first, first_rect, second, second_rect);
    edge_frame(&(input_t){.down = true, .pressed = true, .x = x, .y = y}, first, first_rect, second, second_rect);
    TEST_ASSERT_EQUAL_PTR(mu_get_container(ctx, want), ctx->hover_root);
}

static void
test_a_press_on_a_shared_vertical_edge_reaches_only_the_window_it_is_in(void) {
    const int seam = CANVAS_W / 2;
    const mu_Rect left = mu_rect(0, 0, seam, CANVAS_H);
    const mu_Rect right = mu_rect(seam, 0, CANVAS_W - seam, CANVAS_H);
    assert_seed_lands_in(seam, 100, "right", "right", right, "left", left);
    assert_seed_lands_in(seam - 1, 100, "left", "right", right, "left", left);
}

static void
test_a_press_on_a_shared_horizontal_edge_reaches_only_the_window_it_is_in(void) {
    const int seam = CANVAS_H / 2;
    const mu_Rect top = mu_rect(0, 0, CANVAS_W, seam);
    const mu_Rect bottom = mu_rect(0, seam, CANVAS_W, CANVAS_H - seam);
    assert_seed_lands_in(100, seam, "bottom", "bottom", bottom, "top", top);
    assert_seed_lands_in(100, seam - 1, "top", "bottom", bottom, "top", top);
}

/* However long the finger rests, one press is one click. Holding must not
 * re-fire the control it is resting on. */
static void
test_holding_does_not_resubmit(void) {
    fixture();
    idle_frames(2);

    TEST_ASSERT_EQUAL_INT(1, taps_counted(40));
}

/* A press and release arriving in the SAME frame still clicks: the press
 * frame seeds hover_root, so the DOWN and UP folded into that frame find the
 * control hovered and focus it. */
static void
test_a_one_frame_tap_submits_once(void) {
    fixture();
    idle_frames(2);

    const int cx = BTN_X + BTN_W / 2;
    const int cy = BTN_Y + BTN_H / 2;
    TEST_ASSERT_TRUE(frame(true, true, true, cx, cy));
    TEST_ASSERT_FALSE(frame(false, false, false, cx, cy));
}

static void
test_a_tap_outside_the_button_submits_nothing(void) {
    fixture();
    idle_frames(2);

    int submits = 0;
    if (frame(true, true, false, 10, 400)) {
        submits++;
    }
    for (int i = 0; i < 4; i++) {
        if (frame(true, false, false, 10, 400)) {
            submits++;
        }
    }
    if (frame(false, false, true, 10, 400)) {
        submits++;
    }

    TEST_ASSERT_EQUAL_INT(0, submits);
}

/* The capability the held pointer exists for. */

/* A slider needs mouse_down to persist ACROSS frames - microui only tracks
 * its value while (mouse_down | mouse_pressed) is set. This is what the
 * whole hold policy was introduced for, and it must keep working alongside
 * the hover frames the fix added. */
static void
test_a_drag_moves_a_slider_microui_would_not_track_on_a_tap(void) {
    fixture();
    idle_frames(2);

    mu_Real value = 0;
    const int track_x = BTN_X;
    const int track_w = BTN_W;

    /* Same shape as frame() above, but the control is a slider so the drag
     * has something that only responds while genuinely held. */
    int x = track_x + 10;
    for (int f = 0; f < 12; f++) {
        input_t in = {0};
        in.down = true;
        in.pressed = (f == 0);
        in.x = x;
        in.y = BTN_Y + BTN_H / 2;

        ui_bridge_feed(ctx, &pointer, &in);

        mu_begin(ctx);
        if (mu_begin_window_ex(ctx, "screen", mu_rect(0, 0, CANVAS_W, CANVAS_H),
                               MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
            mu_layout_set_next(ctx, mu_rect(track_x, BTN_Y, track_w, BTN_H), 0);
            mu_slider(ctx, &value, 0, 100);
            mu_end_window(ctx);
        }
        mu_end(ctx);
        ui_bridge_end(ctx, &pointer);

        /* Start dragging only once the press has actually landed, so the
         * movement is a drag and not a series of separate taps. */
        if (f >= UI_POINTER_HOVER_FRAMES) {
            x += 15;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(value > 0, "dragging must move a slider - microui only tracks one while the "
                                        "mouse stays down, which is the reason the pointer holds DOWN at all");
}

/* A screen of rows far taller than the canvas: the shape a list of toggles
 * takes once it outgrows the glass. */

#define LIST_ROWS  20
#define LIST_ROW_H 64

/* One frame of a scrollable list, fed exactly as ui.c does, including the
 * over_scrollable report back after mu_end(). Returns the index of the row
 * whose button submitted, or -1. */
static int
list_frame(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    in.x = x;
    in.y = y;

    ui_bridge_feed(ctx, &pointer, &in);

    int submitted = -1;
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, "list", mu_rect(0, 0, CANVAS_W, CANVAS_H),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        for (int row = 0; row < LIST_ROWS; row++) {
            char label[8];
            label[0] = (char)('A' + row);
            label[1] = '\0';
            mu_layout_row(ctx, 1, (int[]){-1}, LIST_ROW_H);
            if (mu_button(ctx, label)) {
                submitted = row;
            }
        }
        mu_end_window(ctx);
    }
    mu_end(ctx);
    ui_bridge_end(ctx, &pointer);
    return submitted;
}

static void
list_idle_frames(int count) {
    for (int i = 0; i < count; i++) {
        list_frame(false, false, false, 0, 0);
    }
}

/* Drags from (x, y0) to (x, y1) in `steps` frames and lifts; returns how many
 * buttons submitted along the way. */
static int
list_drag(int x, int y0, int y1, int steps) {
    int submits = 0;
    submits += list_frame(true, true, false, x, y0) >= 0;
    for (int i = 1; i <= steps; i++) {
        submits += list_frame(true, false, false, x, y0 + (y1 - y0) * i / steps) >= 0;
    }
    submits += list_frame(false, false, true, x, y1) >= 0;
    return submits;
}

static int
list_tap(int x, int y) {
    int row = -1;
    int r = list_frame(true, true, false, x, y);
    row = r >= 0 ? r : row;
    for (int i = 0; i < 4; i++) {
        r = list_frame(true, false, false, x, y);
        row = r >= 0 ? r : row;
    }
    r = list_frame(false, false, true, x, y);
    return r >= 0 ? r : row;
}

static void
test_dragging_up_scrolls_the_list_without_pressing_a_row(void) {
    fixture();
    list_idle_frames(2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, list_drag(CANVAS_W / 2, 400, 100, 10), "a scrolling drag presses nothing");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, mu_get_container(ctx, "list")->scroll.y,
                                         "a drag up must move the content up");
}

static void
test_a_tap_on_a_scrollable_list_presses_the_row_under_it_once(void) {
    fixture();
    list_idle_frames(2);

    int submits = 0;
    const int cx = CANVAS_W / 2;
    const int cy = 5 + LIST_ROW_H / 2;
    submits += list_frame(true, true, false, cx, cy) >= 0;
    for (int i = 0; i < 4; i++) {
        submits += list_frame(true, false, false, cx, cy) >= 0;
    }
    const int row = list_frame(false, false, true, cx, cy);
    submits += row >= 0;
    list_idle_frames(2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, submits, "a tap still presses exactly once on content that scrolls");
    TEST_ASSERT_EQUAL_INT(0, row);
}

static void
test_scrolling_stops_at_the_end_and_the_last_row_is_reachable(void) {
    fixture();
    list_idle_frames(2);

    for (int i = 0; i < 6; i++) {
        list_drag(CANVAS_W / 2, 420, 20, 8);
        list_idle_frames(1);
    }
    const mu_Container* cnt = mu_get_container(ctx, "list");
    const int scroll_after_overshoot = cnt->scroll.y;
    list_idle_frames(1);
    const int max_scroll = cnt->content_size.y + 2 * ctx->style->padding - cnt->body.h;
    TEST_ASSERT_EQUAL_INT_MESSAGE(max_scroll, cnt->scroll.y, "an overshooting drag clamps to the content's end");
    TEST_ASSERT_EQUAL_INT(scroll_after_overshoot, cnt->scroll.y);

    TEST_ASSERT_EQUAL_INT_MESSAGE(LIST_ROWS - 1, list_tap(CANVAS_W / 2, CANVAS_H - 40),
                                  "the bottom of the glass must now hold the last row");
}

void
run_ui_pointer_microui_suite(void) {
    ctx = malloc(sizeof *ctx);
    RUN_TEST(test_a_tap_submits_the_button_underneath_it);
    RUN_TEST(test_a_tap_submits_on_the_second_frame_of_contact);
    RUN_TEST(test_hover_is_granted_on_the_frame_a_press_lands);
    RUN_TEST(test_a_press_over_stacked_windows_reaches_only_the_top_one);
    RUN_TEST(test_a_press_on_a_shared_vertical_edge_reaches_only_the_window_it_is_in);
    RUN_TEST(test_a_press_on_a_shared_horizontal_edge_reaches_only_the_window_it_is_in);
    RUN_TEST(test_holding_does_not_resubmit);
    RUN_TEST(test_a_one_frame_tap_submits_once);
    RUN_TEST(test_a_tap_outside_the_button_submits_nothing);
    RUN_TEST(test_a_drag_moves_a_slider_microui_would_not_track_on_a_tap);
    RUN_TEST(test_dragging_up_scrolls_the_list_without_pressing_a_row);
    RUN_TEST(test_a_tap_on_a_scrollable_list_presses_the_row_under_it_once);
    RUN_TEST(test_scrolling_stops_at_the_end_and_the_last_row_is_reachable);
    free(ctx);
    ctx = NULL;
}

SUITE_REGISTER(run_ui_pointer_microui_suite);
