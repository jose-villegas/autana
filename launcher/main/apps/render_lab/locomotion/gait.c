/* gait: see gait.h. */
#include "apps/render_lab/locomotion/gait.h"

#include <math.h>

#include "collections/sort.h"
#include "math/scalar/mathf.h"
#include "scene/animator/animator.h"

#define SLOWER(a, b) ((a).ground_mps < (b).ground_mps)
SORT_DEFINE_INSERTION(gait_steps, gait_step_t, SLOWER)

void
gait_sort(gait_step_t* steps, int count) {
    gait_steps_sort(steps, (size_t)count);
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

/* How far a held step reaches past the edge above step i: none at a still step's edge. */
static float
reach(const gait_t* g, int i) {
    return still(g, i) ? 0.0F : g->band;
}

static float
lower_edge(const gait_t* g, int i) {
    return i > 0 ? upper_edge(g, i - 1) : -INFINITY;
}

gait_pick_t
gait_pick(const gait_t* g, gait_pick_t current, float speed) {
    int step = current.step;
    const bool held = step >= 0 && step < g->count && (step == 0 || speed >= lower_edge(g, step) - reach(g, step - 1))
                      && speed < upper_edge(g, step) + reach(g, step);
    if (!held) {
        step = 0;
        while (speed >= upper_edge(g, step)) {
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
