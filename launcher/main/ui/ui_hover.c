/* ui_hover - see ui_hover.h. */
#include "ui/ui_hover.h"

#include <stdbool.h>
#include <stddef.h>

static bool
contains(mu_Rect r, mu_Vec2 p) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

void
ui_hover_seed_root(mu_Context* ctx) {
    mu_Container* top = NULL;
    for (int i = 0; i < ctx->root_list.idx; i++) {
        mu_Container* c = ctx->root_list.items[i];
        if (contains(c->rect, ctx->mouse_pos) && (top == NULL || c->zindex > top->zindex)) {
            top = c;
        }
    }
    ctx->next_hover_root = top;
}
