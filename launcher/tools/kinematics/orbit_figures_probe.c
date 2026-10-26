/* orbit_figures_probe: an orbit's trajectories for the orbit and follow references. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "kinematics/orbit/orbit_motion.h"

static const float frame_s = 0.1F;
static const int fling_half_lives = 4;
static const int home_still_frames = 6;
static const int held_frames = 15;
static const float steering_rate = 1.5F;
static const float pitch_steering_rate = 0.45F;
static const vec3f_t frame_axes[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static const float decay_steps[] = {0.016F, 0.050F};
static const float decay_duration_s = 2.8F;
static const float reset_step_s = 0.01F;
static const float reset_duration_s = 2.0F;
static const float reset_offset = 1.0F;
static const float reset_speeds[] = {0.0F, 6.0F, -20.0F};
static const float sphere_radius = 1.0F;
static const float diagram_half_angle_deg = 30.0F;
static const int fit_min_deg = 10;
static const int fit_max_deg = 80;
static const int fit_step_deg = 2;
static const orbit_motion_state_t home = {0};
static const orbit_motion_limits_t limits = {-ORBIT_MOTION_PITCH_LIMIT, ORBIT_MOTION_PITCH_LIMIT, 0, 0};
static const vec3f_t target = {0};

static orbit_motion_t
new_orbit(void) {
    orbit_motion_t orbit;
    orbit_motion_init(&orbit, target, home, limits);
    return orbit;
}

static void
orbit_samples(void) {
    orbit_motion_t orbit = new_orbit();
    const int reset_frame = held_frames + (int)roundf(fling_half_lives * orbit.fling_half_life_s / frame_s);
    const int maximum_frames = reset_frame + (int)(reset_duration_s / frame_s) + home_still_frames;
    int still_frames = 0;
    printf("\"half_life\":%.9g,\"release\":%.9g,\"reset_start\":%.9g,\"pitch_limit\":%.9g,"
           "\"target\":[%.9g,%.9g,%.9g],\"orbit\":[",
           orbit.fling_half_life_s, held_frames * frame_s, reset_frame * frame_s, ORBIT_MOTION_PITCH_LIMIT, target.x,
           target.y, target.z);
    for (int i = 0; i < maximum_frames; i++) {
        if (i == reset_frame) {
            orbit_motion_update(&orbit, &(orbit_motion_input_t){.reset = true}, 0);
        }
        const transformf_t pose = orbit_motion_pose(&orbit);
        printf("%s{\"time\":%.9g,\"position\":[%.9g,%.9g,%.9g],\"speed\":[%.9g,%.9g],"
               "\"resetting\":%s,\"axes\":[",
               i ? "," : "", i * frame_s, pose.position.x, pose.position.y, pose.position.z, orbit.velocity.yaw,
               orbit.velocity.pitch, orbit.resetting ? "true" : "false");
        for (size_t j = 0; j < sizeof frame_axes / sizeof frame_axes[0]; j++) {
            const vec3f_t axis = quatf_rotate(pose.rotation, frame_axes[j]);
            printf("%s[%.9g,%.9g,%.9g]", j ? "," : "", axis.x, axis.y, axis.z);
        }
        printf("]}");
        if (i >= reset_frame && !orbit.resetting && ++still_frames == home_still_frames) {
            break;
        }
        const orbit_motion_input_t input = {
            .held = i < held_frames, .yaw_turn = steering_rate * frame_s, .pitch_turn = pitch_steering_rate * frame_s};
        orbit_motion_update(&orbit, &input, frame_s);
    }
    if (still_frames != home_still_frames) {
        exit(EXIT_FAILURE);
    }
    printf("],\"decay\":[");
    for (size_t j = 0; j < sizeof decay_steps / sizeof decay_steps[0]; j++) {
        orbit = new_orbit();
        orbit.velocity.yaw = steering_rate;
        const float step = decay_steps[j];
        printf("%s{\"dt\":%.9g,\"limit\":%.9g,\"samples\":[", j ? "," : "", step,
               steering_rate * orbit.fling_half_life_s / MATH_LN2);
        for (int i = 0; i <= (int)(decay_duration_s / step); i++) {
            printf("%s[%.9g,%.9g,%.9g]", i ? "," : "", i * step, orbit.velocity.yaw, orbit.at.yaw);
            orbit_motion_update(&orbit, &(orbit_motion_input_t){0}, step);
        }
        printf("]}");
    }
    printf("],\"reset\":[");
    for (size_t j = 0; j < sizeof reset_speeds / sizeof reset_speeds[0]; j++) {
        orbit = new_orbit();
        orbit.at.yaw = reset_offset;
        orbit.velocity.yaw = reset_speeds[j];
        float settle = -1;
        float minimum = reset_offset;
        printf("%s{\"speed\":%.9g,\"samples\":[", j ? "," : "", reset_speeds[j]);
        for (int i = 0; i <= (int)(reset_duration_s / reset_step_s); i++) {
            printf("%s[%.9g,%.9g]", i ? "," : "", i * reset_step_s, orbit.at.yaw);
            if (orbit.at.yaw < minimum) {
                minimum = orbit.at.yaw;
            }
            orbit_motion_update(&orbit, &(orbit_motion_input_t){.reset = i == 0}, reset_step_s);
            if (!orbit.resetting && settle < 0) {
                settle = (i + 1) * reset_step_s;
            }
        }
        printf("],\"settle\":%.9g,\"minimum\":%.9g}", settle, minimum);
    }
    printf("]");
}

static void
fit_samples(void) {
    const float tangent = tanf(diagram_half_angle_deg * MATH_RADIANS_PER_DEGREE);
    const float padded = tangent * (1 - ORBIT_MOTION_FRAME_PADDING);
    const float distance = orbit_motion_fit_distance(sphere_radius, tangent, ORBIT_MOTION_FRAME_PADDING);
    const float tangent_x = distance / (1 + padded * padded);
    const float tangent_y = tangent_x * padded;
    printf(",\"fit\":{\"radius\":%.9g,\"distance\":%.9g,\"tangent\":[%.9g,%.9g],"
           "\"padding\":%.9g,\"padded_tangent\":%.9g,\"samples\":[",
           sphere_radius, distance, tangent_x, tangent_y, ORBIT_MOTION_FRAME_PADDING, padded);
    for (int angle = fit_min_deg; angle <= fit_max_deg; angle += fit_step_deg) {
        const float t = tanf(angle * MATH_RADIANS_PER_DEGREE);
        printf("%s[%d,%.9g,%.9g]", angle == fit_min_deg ? "" : ",", angle,
               orbit_motion_fit_distance(sphere_radius, t, ORBIT_MOTION_FRAME_PADDING) / sphere_radius,
               orbit_motion_fit_distance(sphere_radius, t, 0) / sphere_radius);
    }
    printf("]}");
}

int
main(void) {
    printf("{");
    orbit_samples();
    fit_samples();
    puts("}");
    return 0;
}
