/* Clip time with integer milliseconds and a fractional speed accumulator. */
#pragma once
#include <stdbool.h>
#include "anim/anim_track.h"

enum { ANIM_PLAYHEAD_SPEED_SHIFT = 16 };

#define ANIM_PLAYHEAD_SPEED_ONE (1U << ANIM_PLAYHEAD_SPEED_SHIFT)

typedef struct {
    uint32_t t_ms;
    uint32_t speed_q16;
    uint16_t fraction;
    anim_wrap_t wrap;
    bool paused;
} anim_playhead_t;

void anim_playhead_init(anim_playhead_t* out, anim_wrap_t wrap);
/* Nonnegative finite speed, bounded by UINT16_MAX; fractional precision is Q16.16. */
bool anim_playhead_set_speed(anim_playhead_t* playhead, float speed);
void anim_playhead_advance(anim_playhead_t* playhead, const anim_clip_t* clip, uint32_t dt_ms);
float anim_playhead_seconds(const anim_playhead_t* playhead);
