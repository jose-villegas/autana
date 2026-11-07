#include "kinematics/fly/fly_motion.h"

#include "math/scalar/mathf.h"

#include <math.h>

#pragma GCC diagnostic error "-Wdouble-promotion"

/* Below this, a forward has no horizontal part to take a heading from. */
#define LEVEL_EPSILON 1.0E-4F

static vec3f_t
level(vec3f_t v) {
    return (vec3f_t){v.x, 0.0F, v.z};
}

/* Where the pose faces across the ground. Looking straight up or down,
 * forward has none, so the picture's top (or its bottom, looking up) says. */
static vec3f_t
heading(const transformf_t* pose) {
    const vec3f_t forward = quatf_rotate(pose->rotation, vec3f_forward());
    if (vec3f_length(level(forward)) >= LEVEL_EPSILON) {
        return level(forward);
    }
    const vec3f_t up = quatf_rotate(pose->rotation, vec3f_up());
    return vec3f_scale(level(up), forward.y < 0.0F ? 1.0F : -1.0F);
}

/* The pose turned by the drag: yaw about the world's up, pitch within the limit, no roll, unit scale. */
static void
turn(transformf_t* pose, vec2f_t look, const fly_motion_tuning_t* tuning) {
    const vec3f_t ahead = heading(pose);
    const vec3f_t forward = quatf_rotate(pose->rotation, vec3f_forward());
    const float yaw = atan2f(ahead.x, ahead.z) + (look.x * tuning->radians_per_pixel);
    const float pitch = mathf_clamp(asinf(mathf_clamp(forward.y, -1.0F, 1.0F)) - (look.y * tuning->radians_per_pixel),
                                    -tuning->pitch_limit, tuning->pitch_limit);
    const vec3f_t facing = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
    *pose = transformf_looking(pose->position, facing, vec3f_up());
}

/* The step `move` asks for over `distance`: along the pose's right and
 * forward and the world's up, no faster along a diagonal. */
static vec3f_t
stride(const transformf_t* pose, vec3f_t move, float distance) {
    const vec3f_t along = vec3f_add(
        vec3f_add(vec3f_scale(quatf_rotate(pose->rotation, vec3f_right()), move.x), vec3f_scale(vec3f_up(), move.y)),
        vec3f_scale(quatf_rotate(pose->rotation, vec3f_forward()), move.z));
    return vec3f_scale(along, distance / fmaxf(vec3f_length(along), 1.0F));
}

fly_motion_t
fly_motion_start(transformf_t pose, float speed, fly_motion_tuning_t tuning) {
    tuning.pitch_limit = fminf(tuning.pitch_limit, FLY_MOTION_PITCH_LIMIT);
    return (fly_motion_t){.pose = pose, .speed = speed, .start_speed = speed, .tuning = tuning};
}

bool
fly_motion_update(fly_motion_t* fly, const fly_motion_steer_t* steer, float dt_s) {
    const fly_motion_tuning_t* tuning = &fly->tuning;
    fly->speed = mathf_clamp(fly->speed * powf(tuning->speed_step, steer->wheel),
                             fly->start_speed * tuning->speed_min_ratio, fly->start_speed * tuning->speed_max_ratio);
    bool changed = false;
    if (steer->look.x != 0.0F || steer->look.y != 0.0F) {
        turn(&fly->pose, steer->look, tuning);
        changed = true;
    }
    const vec3f_t step = stride(&fly->pose, steer->move, fly->speed * dt_s);
    if (vec3f_dot(step, step) > 0.0F) {
        transformf_translate(&fly->pose, step);
        changed = true;
    }
    return changed;
}
