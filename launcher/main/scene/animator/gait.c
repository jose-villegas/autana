/* gait: see gait.h. */
#include "scene/animator/gait.h"

#include <math.h>

#include "collections/sort.h"
#include "math/scalar/mathf.h"
#include "math/scalar/units.h"
#include "scene/animator/animator.h"

#define SLOWER(a, b) ((a).ground_mps < (b).ground_mps)
SORT_DEFINE_INSERTION(gait_steps, gait_step_t, SLOWER)

void
gait_sort(gait_step_t* steps, int count) {
    gait_steps_sort(steps, (size_t)count);
}

int
gait_steps_from_clips(entity_t entity, float scale, float max_mps, gait_step_t* out, int capacity) {
    const int clips = animator_clip_count(entity);
    int count = 0;
    for (int i = 0; i < clips && count < capacity; i++) {
        const float ground = animator_clip_ground_mps(entity, i) * scale;
        if (ground <= max_mps) {
            out[count++] = (gait_step_t){.clip = i, .ground_mps = ground};
        }
    }
    gait_sort(out, count);
    return count;
}

static bool
still(const gait_t* g, int i) {
    return g->steps[i].ground_mps <= 0.0F;
}

/* Where step i's range ends and step i + 1's begins: the stop speed past a
 * still step, else halfway between their ground speeds. */
static float
upper_edge(const gait_t* g, int i) {
    if (i + 1 >= g->count) {
        return INFINITY;
    }
    return still(g, i) ? g->stop_mps : 0.5F * (g->steps[i].ground_mps + g->steps[i + 1].ground_mps);
}

/* Whether `speed` is at or under step i's top edge, a moving step's reaching
 * `band` past it: a still step keeps the stop speed itself, so a scripted
 * stop at exactly 0 with a stop speed of 0 stands. */
static bool
under_top(const gait_t* g, int i, float speed, float band) {
    return still(g, i) ? speed <= g->stop_mps : speed < upper_edge(g, i) + band;
}

gait_pick_t
gait_pick(const gait_t* g, gait_pick_t current, float speed) {
    int step = current.step;
    const bool held = step >= 0 && step < g->count && (step == 0 || !under_top(g, step - 1, speed, -g->band))
                      && under_top(g, step, speed, g->band);
    if (!held) {
        step = 0;
        while (!under_top(g, step, speed, 0.0F)) {
            step++;
        }
    }
    const float ground = g->steps[step].ground_mps;
    const float rate = ground > 0.0F ? mathf_clamp(speed / ground, g->rate_min, g->rate_max) : 1.0F;
    return (gait_pick_t){step, rate};
}

void
gait_apply(entity_t entity, const gait_t* g, gait_pick_t was, gait_pick_t now) {
    if (now.step != was.step) {
        (void)animator_play(entity, g->steps[now.step].clip, g->blend_ms);
    }
    (void)animator_set_speed(entity, ANIMATOR_CURRENT, now.rate);
}

void
gait_hold(entity_t entity, const gait_t* g, gait_pick_t pick, float clip_s, uint32_t cut_ms) {
    const int clip = g->steps[pick.step].clip;
    const uint32_t duration_ms = animator_clip_duration_ms(entity, clip);
    (void)animator_play(entity, clip, cut_ms);
    (void)animator_set_speed(entity, ANIMATOR_CURRENT, pick.rate);
    if (duration_ms > 0) {
        (void)animator_seek(entity, ANIMATOR_CURRENT, (uint32_t)lroundf(clip_s * (float)MATH_MS_PER_S) % duration_ms);
    }
    (void)animator_set_paused(entity, ANIMATOR_CURRENT, true);
}
