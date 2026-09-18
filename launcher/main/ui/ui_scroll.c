/* ui_scroll - see ui_scroll.h. */
#include "ui/ui_scroll.h"

#include <math.h>

#include "ui/ui.h"

ui_scroll_view_config_t
ui_scroll_view_default(void) {
    return (ui_scroll_view_config_t){.axis = UI_SCROLL_AXIS_VERTICAL, .hide_scrollbar = false, .momentum_tau_ms = 0};
}

/* Below this, a coast is over: the residual displacement it could still add
 * rounds to nothing, so ending it here rather than decaying forever avoids
 * both an endless invalidate and a scroll that visibly never quite settles. */
#define UI_SCROLL_MOMENTUM_STOP_PX_PER_MS 0.002f

/* One in-flight coast at a time, matching the shell's own one-window-at-a-
 * time model (main.c: one app, one screen, drawn once per frame). Keyed by
 * container identity so switching screens - a different mu_Container* -
 * starts clean rather than inheriting a stale velocity. */
static const mu_Container* momentum_cnt;
static float momentum_v; /* px/ms, vertical only - see ui_scroll.h */
static int momentum_prev_scroll_y;

static int
clamp_int(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    return (v > hi) ? hi : v;
}

/* Same bound the scrollbar() macro (microui.c) clamps to - content_size and
 * body are last frame's, the same one-frame lag that macro already accepts. */
static int
max_scroll_y(const mu_Context* ctx, const mu_Container* cnt) {
    const int cs_y = cnt->content_size.y + ctx->style->padding * 2;
    const int max_scroll = cs_y - cnt->body.h;
    return (max_scroll > 0) ? max_scroll : 0;
}

/* Applies this frame's momentum coast to `cnt->scroll.y`, if any is owed,
 * and always refreshes the velocity estimate the NEXT coast would start
 * from - continuously, not just at release, so a finger that was already
 * slowing down before it lifted does not coast as though it were still at
 * full speed several frames ago. */
static void
step_momentum(mu_Context* ctx, mu_Container* cnt, uint32_t dt_ms, uint32_t tau_ms) {
    if (cnt != momentum_cnt) {
        momentum_cnt = cnt;
        momentum_v = 0;
        momentum_prev_scroll_y = cnt->scroll.y;
    }

    const bool finger_down = ctx->mouse_down != 0;
    const float dt = (float)dt_ms;

    if (finger_down) {
        momentum_v = (dt > 0) ? (float)(cnt->scroll.y - momentum_prev_scroll_y) / dt : 0;
    } else if (tau_ms > 0 && momentum_v != 0 && dt > 0) {
        const float tau = (float)tau_ms;
        const float decay = expf(-dt / tau);
        const float distance = momentum_v * tau * (1.0f - decay);
        const int max_scroll = max_scroll_y(ctx, cnt);
        cnt->scroll.y = clamp_int(cnt->scroll.y + lroundf(distance), 0, max_scroll);
        momentum_v *= decay;
        if (cnt->scroll.y == 0 || cnt->scroll.y == max_scroll
            || fabsf(momentum_v) < UI_SCROLL_MOMENTUM_STOP_PX_PER_MS) {
            momentum_v = 0;
        }
    }

    momentum_prev_scroll_y = cnt->scroll.y;
}

int
ui_scroll_view_begin(mu_Context* ctx, const char* title, int opt, ui_scroll_view_config_t config, uint32_t dt_ms) {
    const int saved_scrollbar_size = ctx->style->scrollbar_size;
    if (config.hide_scrollbar) {
        ctx->style->scrollbar_size = 0;
    }
    if (config.axis == UI_SCROLL_AXIS_NONE) {
        opt |= MU_OPT_NOSCROLL;
    }

    const int open = ui_begin_screen(ctx, title, opt);
    ctx->style->scrollbar_size = saved_scrollbar_size;
    if (!open) {
        return open;
    }

    mu_Container* cnt = mu_get_current_container(ctx);

    if (config.axis == UI_SCROLL_AXIS_VERTICAL || config.axis == UI_SCROLL_AXIS_BOTH) {
        step_momentum(ctx, cnt, dt_ms, config.momentum_tau_ms);
    }
    if (config.axis != UI_SCROLL_AXIS_VERTICAL && config.axis != UI_SCROLL_AXIS_BOTH) {
        cnt->scroll.y = 0;
    }
    if (config.axis != UI_SCROLL_AXIS_HORIZONTAL && config.axis != UI_SCROLL_AXIS_BOTH) {
        cnt->scroll.x = 0;
    }

    return open;
}

void
ui_scroll_view_end(mu_Context* ctx) {
    mu_end_window(ctx);
}

void
ui_scroll_reset_momentum(void) {
    momentum_cnt = NULL;
    momentum_v = 0;
    momentum_prev_scroll_y = 0;
}

ui_flow_t
ui_flow_start(int canvas_w, int y0, int gap) {
    return (ui_flow_t){.canvas_w = canvas_w, .y = y0, .gap = gap};
}

int
ui_flow_top(int canvas_h, int count, int row_h, int gap, int margin) {
    const int content_h = count * row_h + (count - 1) * gap;
    const int top = (canvas_h - content_h) / 2;
    return (top < margin) ? margin : top;
}

mu_Rect
ui_flow_row(mu_Context* ctx, ui_flow_t* flow, int w, int h) {
    const mu_Rect rest = ui_centered_rect(flow->canvas_w, w, h, flow->y);
    flow->y += h + flow->gap;

    const int p = ctx->style->padding;
    mu_layout_set_next(ctx, (mu_Rect){rest.x - p, rest.y - p, rest.w, rest.h}, 1);
    return rest;
}
