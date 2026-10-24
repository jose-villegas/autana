#include "apps/input_lab/guided_step.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "apps/input_lab/motion_probe.h"
#include "math/scalar/math_const.h"
#include "math/scalar/mathi.h"
#include "ui/ui.h"

#define GUIDE_BOX_Y         107
#define GUIDE_BOX_H         106
#define GUIDE_BOX_MARGIN    18
#define GUIDE_TEXT_H        30
#define GUIDE_RING_Y        260
#define GUIDE_RING_RADIUS   39
#define GUIDE_RING_STEP_DEG 8
#define GUIDE_RING_MARK     4
#define GUIDE_ARROW_MARK    9
#define GUIDE_ANGLE_BOTTOM  32
#define GUIDE_TOP_ANGLE_DEG (-90)
#define GUIDE_ANGLE_H       26
#define GUIDE_INK           0x40E0FF
#define GUIDE_FACE          0x102030
#define GUIDE_DEG_HALF_TURN (MOTION_PROBE_FULL_TURN_DEG / 2)

void
guided_step_begin(guided_step_t* state, int step, int64_t now_us) {
    *state = (guided_step_t){.step = step, .phase_us = now_us, .ready = true};
}

guided_step_event_t
guided_step_update(guided_step_t* state, bool tap, int64_t now_us, int64_t duration_us) {
    if (state->ready) {
        if (!tap) {
            return GUIDED_STEP_WAIT;
        }
        state->ready = false;
        state->phase_us = now_us;
        return GUIDED_STEP_ARMED;
    }
    if (now_us - state->phase_us < duration_us) {
        return GUIDED_STEP_WAIT;
    }
    guided_step_begin(state, state->step + 1, now_us);
    return GUIDED_STEP_NEXT;
}

int64_t
guided_step_remaining_us(const guided_step_t* state, int64_t now_us, int64_t duration_us) {
    if (state->ready) {
        return duration_us;
    }
    const int64_t left = duration_us - (now_us - state->phase_us);
    return left > 0 ? left : 0;
}

static void
guided_step_mark(int x, int y, int side) {
    ui_fill((recti_t){x - side / 2, y - side / 2, side, side}, GUIDE_INK);
}

void
guided_step_draw(const guided_step_t* state, const guided_step_view_t* view) {
    const int delta = mathi_wrap(view->target_bearing_deg - view->current_bearing_deg, 0, MOTION_PROBE_FULL_TURN_DEG);
    const int signed_delta = delta > GUIDE_DEG_HALF_TURN ? delta - MOTION_PROBE_FULL_TURN_DEG : delta;
    const int arrow_delta =
        mathi_wrap(view->target_bearing_deg - view->committed_bearing_deg, 0, MOTION_PROBE_FULL_TURN_DEG);
    const int signed_arrow = arrow_delta > GUIDE_DEG_HALF_TURN ? arrow_delta - MOTION_PROBE_FULL_TURN_DEG : arrow_delta;
    const int direction = signed_arrow < 0 ? -1 : 1;
    const recti_t box = {GUIDE_BOX_MARGIN, GUIDE_BOX_Y, view->width - 2 * GUIDE_BOX_MARGIN, GUIDE_BOX_H};
    ui_fill(box, GUIDE_FACE);
    ui_outline(box, GUIDE_INK);
    char line[96];
    ui_color_begin(UI_COLOR_TEXT, GUIDE_INK);
    ui_scale_begin(2);
    ui_text_centre((recti_t){box.x, box.y + 5, box.w, GUIDE_TEXT_H}, view->action);
    ui_text_centre((recti_t){box.x, box.y + GUIDE_TEXT_H + 7, box.w, GUIDE_TEXT_H},
                   view->target == NULL ? "FACE UP" : view->target);
    if (state->ready) {
        (void)snprintf(line, sizeof line, "TAP WHEN READY");
    } else if (view->target == NULL) {
        (void)snprintf(line, sizeof line, "HOLD FLAT");
    } else {
        (void)snprintf(line, sizeof line, "HOLD AT %d DEG", view->target_tilt_deg);
    }
    ui_text_centre((recti_t){box.x, box.y + 2 * GUIDE_TEXT_H + 9, box.w, GUIDE_TEXT_H}, line);
    ui_scale_end();
    ui_color_end();
    const int cx = view->width / 2;
    const int cy = view->height < GUIDE_RING_Y + GUIDE_RING_RADIUS ? view->height - GUIDE_RING_RADIUS - GUIDE_BOX_MARGIN
                                                                   : GUIDE_RING_Y;
    if (view->target == NULL) {
        (void)snprintf(line, sizeof line, "TILT %d / %d DEG", view->tilt_deg, view->target_tilt_deg);
        ui_text_centre((recti_t){0, view->height - GUIDE_ANGLE_BOTTOM - GUIDE_ANGLE_H, view->width, GUIDE_ANGLE_H},
                       line);
        return;
    }
    const int length = abs(signed_arrow);
    for (int deg = 0; deg <= length; deg += GUIDE_RING_STEP_DEG) {
        const float angle = (float)(GUIDE_TOP_ANGLE_DEG + direction * deg) * MATH_RADIANS_PER_DEGREE;
        guided_step_mark(cx + (int)lroundf(cosf(angle) * GUIDE_RING_RADIUS),
                         cy + (int)lroundf(sinf(angle) * GUIDE_RING_RADIUS), GUIDE_RING_MARK);
    }
    const float tip = (float)(GUIDE_TOP_ANGLE_DEG + signed_arrow) * MATH_RADIANS_PER_DEGREE;
    guided_step_mark(cx + (int)lroundf(cosf(tip) * GUIDE_RING_RADIUS), cy + (int)lroundf(sinf(tip) * GUIDE_RING_RADIUS),
                     GUIDE_ARROW_MARK);
    (void)snprintf(line, sizeof line, "%d DEG %s TO GO   TILT %d / %d DEG", abs(signed_delta),
                   signed_delta < 0 ? "CCW" : "CW", view->tilt_deg, view->target_tilt_deg);
    ui_text_centre((recti_t){0, view->height - GUIDE_ANGLE_BOTTOM - GUIDE_ANGLE_H, view->width, GUIDE_ANGLE_H}, line);
}
