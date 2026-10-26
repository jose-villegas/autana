/* ui_bridge: see ui_bridge.h. */
#include "ui/input/ui_bridge.h"

#include <stddef.h>

static void
replay(mu_Context* ctx, const ui_pointer_event_t* e) {
    switch (e->kind) {
        case UI_POINTER_MOVE: mu_input_mousemove(ctx, e->point.x, e->point.y); break;
        case UI_POINTER_DOWN: mu_input_mousedown(ctx, e->point.x, e->point.y, MU_MOUSE_LEFT); break;
        case UI_POINTER_UP: mu_input_mouseup(ctx, e->point.x, e->point.y, MU_MOUSE_LEFT); break;
        case UI_POINTER_SCROLL: mu_input_scroll(ctx, e->point.x, e->point.y); break;
    }
}

void
ui_bridge_feed(mu_Context* ctx, ui_pointer_t* p, const input_t* input) {
    ui_pointer_event_t events[UI_POINTER_MAX_EVENTS];
    const int n = ui_pointer_step(p, input, events, UI_POINTER_MAX_EVENTS);
    for (int i = 0; i < n; i++) {
        replay(ctx, &events[i]);
    }
    if (input->pressed) {
        mu_seed_hover_root(ctx);
    }
}

void
ui_bridge_end(mu_Context* ctx, ui_pointer_t* p) {
    p->over_scrollable = ctx->scroll_target != NULL;
    p->hover_unsettled = ctx->hover_root != ctx->next_hover_root;
}
