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
    int n = emit(out, 0, UI_POINTER_MOVE, p->press_x, p->press_y);
    if (input->released) {
        n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
        n = emit(out, n, UI_POINTER_UP, p->press_x, p->press_y);
        p->press_deferred = false;
        return n;
    }
    if (distance(input->x, p->press_x) > UI_POINTER_DRAG_THRESHOLD) {
        n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
        p->press_deferred = false;
        p->down = true;
    }
    return n;
}

/* First of the hover frames: position only - see UI_POINTER_HOVER_FRAMES for
 * why one frame is not enough. A tap resolved within this frame still owes
 * a down/up pair, fed here so microui sees the same mouse_down-already-clear
 * frame it resolved fast taps on before. */
static int
step_pressed(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    p->press_deferred = false;
    p->dragging = false;
    p->press_stage = 1;
    p->press_x = input->x;
    p->press_y = input->y;
    int n = emit(out, 0, UI_POINTER_MOVE, p->press_x, p->press_y);

    if (!input->released) {
        return n;
    }
    n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
    n = emit(out, n, UI_POINTER_UP, input->x, input->y);
    p->press_stage = 0;
    p->down = false;
    return n;
}

static int
step_hover(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out) {
    if (p->over_scrollable && !input->released && moved_vertically_past_threshold(p, input)) {
        return begin_drag(p, input, out, 0);
    }
    int n = emit(out, 0, UI_POINTER_MOVE, p->press_x, p->press_y);

    const bool last_hover_frame = (p->press_stage >= UI_POINTER_HOVER_FRAMES);
    if (last_hover_frame && p->over_scrollable && !input->released) {
        p->press_stage = 0;
        p->press_deferred = true;
    } else if (last_hover_frame) {
        n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
        p->press_stage = 0;
        p->down = true;
    } else {
        p->press_stage++;
    }

    if (!input->released) {
        return n;
    }
    /* Lifted mid-sequence. A press that never got its DOWN still owes one,
     * or the tap vanishes entirely. */
    if (!p->down) {
        n = emit(out, n, UI_POINTER_DOWN, p->press_x, p->press_y);
    }
    n = emit(out, n, UI_POINTER_UP, input->x, input->y);
    p->press_stage = 0;
    p->down = false;
    return n;
}

int
ui_pointer_step(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out, int max) {
    if (max < UI_POINTER_MAX_EVENTS) {
        return 0;
    }

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
