/* motion_profile: see motion_profile.h. Each ramp's speed is a smoothstep of
 * its time, so its distance is the smoothstep's integral. */
#include "kinematics/profile/motion_profile.h"

#include <assert.h>

#include "math/scalar/ease.h"

/* The share of a ramp's distance covered `u` (0 to 1) of the way through it
 * easing up: the integral of 3u^2 - 2u^3, over its whole, 1/2. */
static float
ramp_up_share(float u) {
    return 2.0F * (u * u * u - (0.5F * u * u * u * u));
}

float
motion_profile_cruise_speed(const motion_profile_t* p) {
    assert(p->end > p->start && p->ramp_s >= 0.0F);
    const float distance = p->end - p->start;
    /* Each ramp covers half its time at the cruise speed. */
    return p->ramp_s > 0.0F && p->peak_speed * p->ramp_s > distance ? distance / p->ramp_s : p->peak_speed;
}

/* The cruise's time between the ramps. */
static float
cruise_s(const motion_profile_t* p) {
    const float speed = motion_profile_cruise_speed(p);
    const float left = (p->end - p->start) - (speed * p->ramp_s);
    return speed > 0.0F && left > 0.0F ? left / speed : 0.0F;
}

float
motion_profile_duration_s(const motion_profile_t* p) {
    return (2.0F * p->rest_s) + (2.0F * p->ramp_s) + cruise_s(p);
}

motion_profile_at_t
motion_profile_at(const motion_profile_t* p, float t_s) {
    const float speed = motion_profile_cruise_speed(p);
    const float half_ramp = 0.5F * speed * p->ramp_s;
    const float cruise = cruise_s(p);
    float t = t_s - p->rest_s;
    if (t <= 0.0F) {
        return (motion_profile_at_t){p->start, 0.0F};
    }
    if (t < p->ramp_s) {
        const float u = t / p->ramp_s;
        return (motion_profile_at_t){p->start + (half_ramp * ramp_up_share(u)), speed * ease_smoothstep(0.0F, 1.0F, u)};
    }
    t -= p->ramp_s;
    if (t < cruise) {
        return (motion_profile_at_t){p->start + half_ramp + (speed * t), speed};
    }
    t -= cruise;
    if (t < p->ramp_s) {
        /* Easing down is easing up run backwards from the end. */
        const float u = 1.0F - (t / p->ramp_s);
        return (motion_profile_at_t){p->end - (half_ramp * ramp_up_share(u)), speed * ease_smoothstep(0.0F, 1.0F, u)};
    }
    return (motion_profile_at_t){p->end, 0.0F};
}
