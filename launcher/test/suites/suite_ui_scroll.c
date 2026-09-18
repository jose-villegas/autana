/*
 * Portable suite: ui_scroll's shared scroll view - the API a boot menu, the
 * launcher list and a runtime-options menu all migrated onto, replacing
 * three private copies of the same RELATIVE-row fix. Drives real microui
 * through ui_pointer_step(), the same bridge suite_ui_pointer_microui.c
 * proves against a plain list, so these assertions cover ui_flow_row() and
 * ui_scroll_view_begin()/_end() actually reaching a container's own
 * scrolling rather than merely computing rects that would.
 *
 * The axis-lock and momentum tests drive microui directly instead
 * (mu_input_mousemove()/mu_input_scroll(), no ui_pointer_step()): both are
 * about what ui_scroll.c itself does with a scroll delta once one arrives,
 * not about whether a touch gesture produces one - already covered by the
 * drag-based tests above and by suite_ui_pointer_microui.c.
 */

#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx.h"
#include "microui.h"
#include "ui/ui.h"
#include "ui/ui_pointer.h"
#include "ui/ui_scroll.h"
#include "ui/ui_transform.h"

#define ROWS    20
#define ROW_W   300
#define ROW_H   UI_ROW_HEIGHT
#define ROW_GAP 8

static mu_Context* ctx;
static ui_pointer_t pointer;
static ui_scroll_view_config_t config;

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
set_landscape(void) {
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
}

static void
fixture(void) {
    TEST_ASSERT_NOT_NULL(ctx);
    memset(ctx, 0, sizeof *ctx);
    memset(&pointer, 0, sizeof pointer);
    mu_init(ctx);
    ctx->text_width = stub_text_width;
    ctx->text_height = stub_text_height;
    set_landscape();
    config = ui_scroll_view_default();
    ui_scroll_reset_momentum();
}

static void
build_rows(uint32_t dt_ms, int* submitted) {
    if (ui_scroll_view_begin(ctx, "scroll", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME, config,
                             dt_ms)) {
        ui_flow_t flow = ui_flow_start(ui_width(), 0, ROW_GAP);
        for (int row = 0; row < ROWS; row++) {
            ui_flow_row(ctx, &flow, ROW_W, ROW_H);
            char label[8];
            label[0] = (char)('A' + row);
            label[1] = '\0';
            if (mu_button(ctx, label) && submitted != NULL) {
                *submitted = row;
            }
        }
        ui_scroll_view_end(ctx);
    }
}

/* One frame of the real bridge, fed exactly as ui.c does: touch through
 * ui_pointer_step(), a full-screen window of ROWS flowing rows, closed
 * through ui_scroll_view_end(). Returns which row submitted, or -1. */
static int
scroll_frame(bool down, bool pressed, bool released, int x, int y, uint32_t dt_ms) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    in.x = x;
    in.y = y;

    ui_pointer_event_t ev[UI_POINTER_MAX_EVENTS];
    const int n = ui_pointer_step(&pointer, &in, ev, UI_POINTER_MAX_EVENTS);
    for (int i = 0; i < n; i++) {
        switch (ev[i].kind) {
            case UI_POINTER_MOVE: mu_input_mousemove(ctx, ev[i].x, ev[i].y); break;
            case UI_POINTER_DOWN: mu_input_mousedown(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT); break;
            case UI_POINTER_UP: mu_input_mouseup(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT); break;
            case UI_POINTER_SCROLL: mu_input_scroll(ctx, ev[i].x, ev[i].y); break;
        }
    }

    int submitted = -1;
    mu_begin(ctx);
    build_rows(dt_ms, &submitted);
    mu_end(ctx);
    pointer.over_scrollable = ctx->scroll_target != NULL;
    return submitted;
}

/* A frame built directly against microui - no ui_pointer_step(), so the
 * caller controls mouse_down and any scroll delta exactly. */
static void
raw_frame(int mouse_x, int mouse_y, bool mouse_down, uint32_t dt_ms) {
    mu_input_mousemove(ctx, mouse_x, mouse_y);
    ctx->mouse_down = mouse_down ? MU_MOUSE_LEFT : 0;
    mu_begin(ctx);
    build_rows(dt_ms, NULL);
    mu_end(ctx);
}

static void
idle_frames(int count) {
    for (int i = 0; i < count; i++) {
        scroll_frame(false, false, false, 0, 0, 16);
    }
}

static int
drag(int x, int y0, int y1, int steps) {
    int submits = 0;
    submits += scroll_frame(true, true, false, x, y0, 16) >= 0;
    for (int i = 1; i <= steps; i++) {
        submits += scroll_frame(true, false, false, x, y0 + (y1 - y0) * i / steps, 16) >= 0;
    }
    submits += scroll_frame(false, false, true, x, y1, 16) >= 0;
    return submits;
}

static int
tap(int x, int y) {
    int row = -1;
    int r = scroll_frame(true, true, false, x, y, 16);
    row = r >= 0 ? r : row;
    for (int i = 0; i < 4; i++) {
        r = scroll_frame(true, false, false, x, y, 16);
        row = r >= 0 ? r : row;
    }
    r = scroll_frame(false, false, true, x, y, 16);
    return r >= 0 ? r : row;
}

static void
test_rows_flow_and_the_container_reports_content_height(void) {
    fixture();
    idle_frames(2);

    const mu_Container* cnt = mu_get_container(ctx, "scroll");
    /* mu_layout_next()'s RELATIVE path folds a row's already-padding-
     * corrected y into content_size by re-adding, then pop_container()
     * subtracts the container's own body.y (== padding at rest) back out
     * once - see ui_flow_row()'s own comment. */
    const int expected = ROWS * ROW_H + (ROWS - 1) * ROW_GAP - ctx->style->padding;
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, cnt->content_size.y,
                                  "a RELATIVE flowed row must fold into content_size - an ABSOLUTE "
                                  "rect (the bug this API replaces) reports none of it");
}

static void
test_the_last_row_of_an_overflowing_list_is_reachable_by_dragging(void) {
    fixture();
    idle_frames(2);

    for (int i = 0; i < 6; i++) {
        drag(ui_width() / 2, ui_height() - 20, 20, 8);
        idle_frames(1);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(ROWS - 1, tap(ui_width() / 2, ui_height() - 40),
                                  "the bottom of the glass must now hold the last row");
}

static void
test_a_tap_still_presses_exactly_one_row(void) {
    fixture();
    idle_frames(2);

    const mu_Rect first = ui_centered_rect(ui_width(), ROW_W, ROW_H, 0);
    TEST_ASSERT_EQUAL_INT(0, tap(first.x + first.w / 2, first.y + first.h / 2));
}

static void
test_a_hidden_scrollbar_still_scrolls(void) {
    fixture();
    config.hide_scrollbar = true;
    idle_frames(2);

    int presses = 0;
    for (int i = 0; i < 6; i++) {
        presses += drag(ui_width() / 2, ui_height() - 20, 20, 8);
        idle_frames(1);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, presses, "a scrolling drag presses nothing");
    const mu_Container* cnt = mu_get_container(ctx, "scroll");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, cnt->scroll.y, "a hidden scrollbar must still let content scroll");
}

/* Settles hover_root over the window's body - mu_mouse_over() needs a
 * hover_root from the PREVIOUS frame (mu_begin() copies it), so nothing
 * before the second of these frames can ever become a scroll_target. */
static void
settle_over_body(void) {
    raw_frame(ui_width() / 2, ui_height() / 2, false, 16);
    raw_frame(ui_width() / 2, ui_height() / 2, false, 16);
}

static void
test_an_axis_locked_view_refuses_the_other_axis(void) {
    fixture();
    config.axis = UI_SCROLL_AXIS_VERTICAL;
    settle_over_body();

    /* mu_end() (called after this suite's raw_frame() returns) is what
     * actually applies a scroll delta, so the axis lock - applied at the
     * TOP of the next window build, before layout uses it - needs one more
     * frame to zero it back out. The same one-frame lag microui's own
     * scrollbar clamp already runs on. */
    mu_input_scroll(ctx, 40, 0);
    raw_frame(ui_width() / 2, ui_height() / 2, false, 16);
    raw_frame(ui_width() / 2, ui_height() / 2, false, 16);
    const mu_Container* cnt = mu_get_container(ctx, "scroll");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cnt->scroll.x, "a vertical-only view must refuse a horizontal delta");

    mu_input_scroll(ctx, 0, 40);
    raw_frame(ui_width() / 2, ui_height() / 2, false, 16);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, cnt->scroll.y, "the allowed axis must still move");
}

/* The smoothing knob: a coast must cover the same distance over the same
 * elapsed time regardless of how that time is split into frames - the
 * closed-form integral ui_scroll.c's step_momentum() uses, not a per-frame
 * fixed fraction, is what this pins down. */
static int
run_coast(uint32_t frame_dt_ms, int frame_count) {
    fixture();
    config.momentum_tau_ms = 120;

    const int cx = ui_width() / 2;
    const int cy = ui_height() / 2;
    settle_over_body();

    /* An 80px move over 16ms while held, giving step_momentum() a known,
     * deterministic v0 = 5 px/ms to start the coast from - no touch-state
     * warm-up involved, since that is what the drag-based tests above
     * already prove reachable. */
    raw_frame(cx, cy, true, 16);
    mu_get_container(ctx, "scroll")->scroll.y += 80;
    raw_frame(cx, cy, true, 16);
    raw_frame(cx, cy, false, 16); /* release: coasts one more 16ms step too, same for every call */

    for (int i = 0; i < frame_count; i++) {
        raw_frame(cx, cy, false, frame_dt_ms);
    }
    return mu_get_container(ctx, "scroll")->scroll.y;
}

static void
test_the_smoothing_knob_output_is_a_function_of_dt(void) {
    const int scroll_60fps = run_coast(16, 20); /* 20 * 16ms = 320ms total */
    const int scroll_30fps = run_coast(32, 10); /* 10 * 32ms = 320ms total */

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, scroll_60fps, "the coast must actually move the content");
    TEST_ASSERT_INT_WITHIN_MESSAGE(2, scroll_60fps, scroll_30fps,
                                   "the same drag at 30 and 60 fps must coast the same distance");
}

void
run_ui_scroll_suite(void) {
    ctx = malloc(sizeof *ctx);
    RUN_TEST(test_rows_flow_and_the_container_reports_content_height);
    RUN_TEST(test_the_last_row_of_an_overflowing_list_is_reachable_by_dragging);
    RUN_TEST(test_a_tap_still_presses_exactly_one_row);
    RUN_TEST(test_a_hidden_scrollbar_still_scrolls);
    RUN_TEST(test_an_axis_locked_view_refuses_the_other_axis);
    RUN_TEST(test_the_smoothing_knob_output_is_a_function_of_dt);
    free(ctx);
    ctx = NULL;
}

SUITE_REGISTER(run_ui_scroll_suite);
