/* ui_pointer - see ui_pointer.h. */
#include "ui/ui_pointer.h"

static ui_pointer_event_t
make(ui_pointer_kind_t kind, int x, int y) {
    return (ui_pointer_event_t){.kind = kind, .x = x, .y = y};
}

/* No bounds check: ui_pointer_step()'s max < UI_POINTER_MAX_EVENTS guard
 * already rejected any buffer too small for the most this can ever write. */
static int
emit(ui_pointer_event_t* out, int n, ui_pointer_kind_t kind, int x, int y) {
    out[n] = make(kind, x, y);
    return n + 1;
}

static int
distance(int a, int b) {
    return a > b ? a - b : b - a;
}

void
ui_pointer_aim(ui_pointer_t* p, int x, int y) {
    p->aim_x = x;
    p->aim_y = y;
    p->aimed = true;
}

static bool
moved_vertically_past_threshold(const ui_pointer_t* p, const input_t* input) {
    const int dy = distance(input->y, p->press_y);
    return dy > UI_POINTER_DRAG_THRESHOLD && dy >= distance(input->x, p->press_x);
}

static int
begin_drag(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out, int n) {
    p->press_stage = 0;
    p->press_deferred = false;
    p->dragging = true;
    p->aimed = false;
    p->last_x = input->x;
    p->last_y = input->y;
    n = emit(out, n, UI_POINTER_MOVE, input->x, input->y);
    return emit(out, n, UI_POINTER_SCROLL, p->press_x - input->x, p->press_y - input->y);
}

static int
step_drag(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    int n = emit(out, 0, UI_POINTER_MOVE, input->x, input->y);
    if (input->released) {
        p->dragging = false;
        return n;
    }
    if (input->x != p->last_x || input->y != p->last_y) {
        n = emit(out, n, UI_POINTER_SCROLL, p->last_x - input->x, p->last_y - input->y);
        p->last_x = input->x;
        p->last_y = input->y;
    }
    return n;
}

/* A press on scrollable content whose hover frames have played out: a
 * release is the tap, a sideways move hands the press to the control. */
static int
step_deferred(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    if (!input->released && moved_vertically_past_threshold(p, input)) {
        return begin_drag(p, input, out, 0);
    }
    int n = emit(out, 0, UI_POINTER_MOVE, p->aim_x, p->aim_y);
    if (input->released) {
        n = emit(out, n, UI_POINTER_DOWN, p->aim_x, p->aim_y);
        n = emit(out, n, UI_POINTER_UP, input->x, input->y);
        p->press_deferred = false;
        p->aimed = false;
        return n;
    }
    if (distance(input->x, p->press_x) > UI_POINTER_DRAG_THRESHOLD) {
        n = emit(out, n, UI_POINTER_DOWN, p->aim_x, p->aim_y);
        p->press_deferred = false;
        p->down = true;
        p->aimed = false;
    }
    return n;
}

/* The press frame: position only, then the hover frames of
 * UI_POINTER_HOVER_FRAMES. A tap resolved within this frame still owes a
 * down/up pair, fed here so microui sees a mouse_down-already-clear frame. */
static int
step_pressed(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    p->press_deferred = false;
    p->dragging = false;
    p->press_stage = 1;
    p->press_x = input->x;
    p->press_y = input->y;
    if (!p->aimed) {
        p->aim_x = p->press_x;
        p->aim_y = p->press_y;
    }
    int n = emit(out, 0, UI_POINTER_MOVE, p->aim_x, p->aim_y);

    if (!input->released) {
        return n;
    }
    n = emit(out, n, UI_POINTER_DOWN, p->aim_x, p->aim_y);
    n = emit(out, n, UI_POINTER_UP, input->x, input->y);
    p->press_stage = 0;
    p->down = false;
    p->aimed = false;
    return n;
}

static int
step_hover(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    if (p->over_scrollable && !input->released && moved_vertically_past_threshold(p, input)) {
        return begin_drag(p, input, out, 0);
    }
    int n = emit(out, 0, UI_POINTER_MOVE, p->aim_x, p->aim_y);

    const bool last_hover_frame = (p->press_stage >= UI_POINTER_HOVER_FRAMES && !p->hover_unsettled);
    if (last_hover_frame && p->over_scrollable && !input->released) {
        p->press_stage = 0;
        p->press_deferred = true;
    } else if (last_hover_frame) {
        n = emit(out, n, UI_POINTER_DOWN, p->aim_x, p->aim_y);
        p->press_stage = 0;
        p->down = true;
        p->aimed = false;
    } else if (p->press_stage < UINT8_MAX) {
        p->press_stage++;
    }

    if (!input->released) {
        return n;
    }
    /* Lifted mid-sequence. A press that never got its DOWN still owes one,
     * or the tap vanishes entirely. */
    if (!p->down) {
        n = emit(out, n, UI_POINTER_DOWN, p->aim_x, p->aim_y);
    }
    n = emit(out, n, UI_POINTER_UP, input->x, input->y);
    p->press_stage = 0;
    p->down = false;
    p->aimed = false;
    return n;
}

static int
step_input(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    int n = 0;

    if (input->pressed) {
        return step_pressed(p, input, out);
    }

    if (p->dragging) {
        return step_drag(p, input, out);
    }

    if (p->press_deferred) {
        return step_deferred(p, input, out);
    }

    if (p->press_stage > 0) {
        return step_hover(p, input, out);
    }

    if (input->released) {
        n = emit(out, n, UI_POINTER_MOVE, input->x, input->y);
        if (p->down) {
            /* Only a DOWN we actually emitted needs a matching UP - a
             * finger already on the glass when the UI opened never got
             * one (see input->down below), so its lift must stay silent. */
            n = emit(out, n, UI_POINTER_UP, input->x, input->y);
        }
        p->down = false;
        return n;
    }

    if (input->down) {
        /* A drag reads its position here every frame in between; also
         * covers a finger already down when the UI opened - no `pressed`
         * edge was ever seen for it, so no DOWN is synthesized for it
         * either, only this move. */
        n = emit(out, n, UI_POINTER_MOVE, input->x, input->y);
        return n;
    }

    /* Nothing down, nothing pending: park the pointer off-screen so no
     * control sits hovered. */
    n = emit(out, n, UI_POINTER_MOVE, -1, -1);
    return n;
}

/* A DOWN still unanswered when the finger is gone, or a new press arrives,
 * means its lift went to a frame that never reached microui. Left held it
 * would keep every later control from being hovered, so the UP is paid first. */
int
ui_pointer_step(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out, int max) {
    if (max < UI_POINTER_MAX_EVENTS) {
        return 0;
    }
    int owed = 0;
    if (p->down && (input->pressed || !(input->down || input->released))) {
        owed = emit(out, 0, UI_POINTER_UP, -1, -1);
        p->down = false;
    }
    return owed + step_input(p, input, out + owed);
}
