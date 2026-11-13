/* capybara_run: see capybara_run.h. Each ramp's speed is a smoothstep of
 * its time, so its distance is the smoothstep's integral. */
#include "apps/render_lab/capybara_sponza/capybara_run.h"

#include "math/scalar/math_const.h"
#include "math/scalar/mathf.h"
#include "math/scalar/units.h"

static float
seconds(uint32_t ms) {
    return (float)ms / (float)MATH_MS_PER_S;
}

/* The share of a ramp's distance covered `u` (0 to 1) of the way through it
 * easing up: the integral of 3u^2 - 2u^3, over its whole, 1/2. */
static float
ramp_up_share(float u) {
    return 2.0F * (u * u * u - (0.5F * u * u * u * u));
}

float
capybara_run_cruise_speed(const capybara_run_t* run) {
    const float distance = run->end - run->start;
    const float ramps_s = seconds(run->ramp_ms);
    /* Each ramp covers half its time at the cruise speed. */
    return ramps_s > 0.0F && run->peak_speed * ramps_s > distance ? distance / ramps_s : run->peak_speed;
}

/* The cruise's time between the ramps. */
static float
cruise_s(const capybara_run_t* run) {
    const float speed = capybara_run_cruise_speed(run);
    const float left = (run->end - run->start) - (speed * seconds(run->ramp_ms));
    return speed > 0.0F && left > 0.0F ? left / speed : 0.0F;
}

uint32_t
capybara_run_duration_ms(const capybara_run_t* run) {
    return (2U * run->idle_ms) + (2U * run->ramp_ms) + (uint32_t)(cruise_s(run) * (float)MATH_MS_PER_S);
}

capybara_run_at_t
capybara_run_at(const capybara_run_t* run, uint32_t t_ms) {
    const float speed = capybara_run_cruise_speed(run);
    const float ramp_s = seconds(run->ramp_ms);
    const float half_ramp = 0.5F * speed * ramp_s;
    const float cruise = cruise_s(run);
    float t = seconds(t_ms) - seconds(run->idle_ms);
    if (t <= 0.0F) {
        return (capybara_run_at_t){run->start, 0.0F};
    }
    if (t < ramp_s) {
        const float u = t / ramp_s;
        return (capybara_run_at_t){run->start + (half_ramp * ramp_up_share(u)),
                                   speed * mathf_smoothstep(0.0F, 1.0F, u)};
    }
    t -= ramp_s;
    if (t < cruise) {
        return (capybara_run_at_t){run->start + half_ramp + (speed * t), speed};
    }
    t -= cruise;
    if (t < ramp_s) {
        /* Easing down is easing up run backwards from the end. */
        const float u = 1.0F - (t / ramp_s);
        return (capybara_run_at_t){run->end - (half_ramp * ramp_up_share(u)), speed * mathf_smoothstep(0.0F, 1.0F, u)};
    }
    return (capybara_run_at_t){run->end, 0.0F};
}

float
capybara_run_camera_angle(const capybara_run_t* run, float speed) {
    const float cruise = capybara_run_cruise_speed(run);
    return cruise > 0.0F ? MATH_PI * mathf_smoothstep(0.0F, cruise, speed) : 0.0F;
}
