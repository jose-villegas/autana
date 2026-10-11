#include "anim/playback/anim_playhead.h"
#include <math.h>

#include "math/scalar/mathi.h"

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
    const uint64_t elapsed_ms = p->t_ms + (ticks >> ANIM_PLAYHEAD_SPEED_SHIFT);
    const uint32_t duration_ms = clip->duration_ms;
    p->t_ms = duration_ms == 0 ? 0
                               : (uint32_t)(p->wrap == ANIM_LOOP ? mathi_repeat_u64(elapsed_ms, duration_ms)
                                                                 : mathi_clamp_u64(elapsed_ms, 0, duration_ms));
}

float
anim_playhead_seconds(const anim_playhead_t* p, const anim_clip_t* clip) {
    return anim_clip_seconds(clip, p->t_ms, p->wrap);
}
