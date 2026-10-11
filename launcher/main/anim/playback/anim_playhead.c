#include "anim/playback/anim_playhead.h"
#include <math.h>

static uint32_t
clip_time(uint64_t t_ms, const anim_clip_t* clip, anim_wrap_t wrap) {
    const uint32_t duration = clip->duration_ms;
    if (duration == 0) {
        return 0;
    }
    if (wrap == ANIM_LOOP) {
        return (uint32_t)(t_ms % duration);
    }
    return t_ms > duration ? duration : (uint32_t)t_ms;
}

void
anim_playhead_init(anim_playhead_t* out, anim_wrap_t wrap) {
    *out = (anim_playhead_t){.speed_q16 = ANIM_PLAYHEAD_SPEED_ONE, .wrap = wrap};
}

bool
anim_playhead_set_speed(anim_playhead_t* p, float speed) {
    if (!isfinite(speed) || speed < 0.0F || speed > (float)UINT16_MAX) {
        return false;
    }
    p->speed_q16 = (uint32_t)(speed * (float)ANIM_PLAYHEAD_SPEED_ONE);
    return true;
}

void
anim_playhead_advance(anim_playhead_t* p, const anim_clip_t* clip, uint32_t dt_ms) {
    if (p->paused) {
        return;
    }
    const uint64_t ticks = ((uint64_t)dt_ms * p->speed_q16) + p->fraction;
    p->fraction = (uint16_t)(ticks & (ANIM_PLAYHEAD_SPEED_ONE - 1U));
    p->t_ms = clip_time(p->t_ms + (ticks >> ANIM_PLAYHEAD_SPEED_SHIFT), clip, p->wrap);
}

float
anim_playhead_seconds(const anim_playhead_t* p, const anim_clip_t* clip) {
    return anim_clip_seconds(clip, p->t_ms, p->wrap);
}
