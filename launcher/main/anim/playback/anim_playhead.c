#include "anim/playback/anim_playhead.h"
#include <math.h>

#include "math/scalar/mathi.h"

static const float SECONDS_PER_MILLISECOND = 0.001F;

void
anim_playhead_init(anim_playhead_t* out, anim_wrap_t wrap) {
    *out = (anim_playhead_t){.speed_q16 = ANIM_PLAYHEAD_SPEED_ONE, .wrap = wrap};
}

bool
anim_playhead_speed_q16(float speed, uint32_t* out_q16) {
    if (!isfinite(speed) || speed < 0.0F || speed > (float)ANIM_PLAYHEAD_SPEED_MAX) {
        return false;
    }
    *out_q16 = (uint32_t)(speed * (float)ANIM_PLAYHEAD_SPEED_ONE);
    return true;
}

bool
anim_playhead_set_speed(anim_playhead_t* p, float speed) {
    return anim_playhead_speed_q16(speed, &p->speed_q16);
}

static uint32_t
clip_time(anim_wrap_t wrap, uint64_t elapsed_ms, uint32_t duration_ms) {
    if (duration_ms == 0) {
        return 0;
    }
    return wrap == ANIM_LOOP ? mathi_repeat_u64(elapsed_ms, duration_ms) : mathi_clamp_u64(elapsed_ms, 0, duration_ms);
}

void
anim_playhead_seek(anim_playhead_t* p, const anim_clip_t* clip, uint64_t t_ms) {
    p->t_ms = clip_time(p->wrap, t_ms, clip->duration_ms);
    p->fraction = 0;
}

bool
anim_playhead_advance(anim_playhead_t* p, const anim_clip_t* clip, uint32_t dt_ms) {
    const uint64_t ticks = (p->paused ? 0 : (uint64_t)dt_ms * p->speed_q16) + p->fraction;
    p->fraction = (uint16_t)(ticks & (ANIM_PLAYHEAD_SPEED_ONE - 1U));
    const uint64_t elapsed_ms = p->t_ms + (ticks >> ANIM_PLAYHEAD_SPEED_SHIFT);
    const uint32_t duration_ms = clip->duration_ms;
    p->t_ms = clip_time(p->wrap, elapsed_ms, duration_ms);
    return p->wrap == ANIM_LOOP && !p->paused && duration_ms != 0 && elapsed_ms >= duration_ms;
}

float
anim_playhead_seconds(const anim_playhead_t* p) {
    return (float)p->t_ms * SECONDS_PER_MILLISECOND;
}

float
anim_playhead_sample_seconds(const anim_playhead_t* p, const anim_clip_t* clip) {
    /* The rounded clip duration can precede a track's final key. */
    return p->wrap == ANIM_CLAMP && p->t_ms == clip->duration_ms ? INFINITY : anim_playhead_seconds(p);
}
