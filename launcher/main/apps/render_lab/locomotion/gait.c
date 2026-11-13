/* gait: see gait.h. */
#include "apps/render_lab/locomotion/gait.h"

#include <math.h>

#include "scene/animator/animator.h"

/* Where step i's range ends and step i + 1's begins: halfway between their ground speeds. */
static float
upper_edge(const gait_t* g, int i) {
    return i + 1 < g->count ? 0.5F * (g->steps[i].ground_mps + g->steps[i + 1].ground_mps) : INFINITY;
}

static float
lower_edge(const gait_t* g, int i) {
    return i > 0 ? upper_edge(g, i - 1) : -INFINITY;
}

gait_pick_t
gait_pick(const gait_t* g, gait_pick_t current, float speed) {
    int step = current.step;
    const bool held =
        step >= 0 && step < g->count && speed >= lower_edge(g, step) - g->band && speed < upper_edge(g, step) + g->band;
    if (!held) {
        step = 0;
        while (speed >= upper_edge(g, step)) {
            step++;
        }
    }
    const float ground = g->steps[step].ground_mps;
    const float rate = ground > 0.0F ? fminf(g->rate_max, fmaxf(g->rate_min, speed / ground)) : 1.0F;
    return (gait_pick_t){step, rate};
}

void
gait_apply(entity_t entity, const gait_t* g, gait_pick_t was, gait_pick_t now) {
    if (now.step != was.step) {
        (void)animator_play(entity, g->steps[now.step].clip, g->blend_ms);
    }
    (void)animator_set_speed(entity, ANIMATOR_CURRENT, now.rate);
}
