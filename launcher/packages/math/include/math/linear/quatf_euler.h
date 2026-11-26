/*
 * quatf_euler: a float rotation read back as the Euler angles quatf_from_euler takes.
 */
#pragma once

#include <math.h>

#include "math/linear/quatf.h"
#include "math/linear/vec3f.h"

/* How small cos pitch gets before pitch is taken as +-90 degrees (about 0.06
 * degrees short); the angles read inside it turn an axis at most this far off. */
#define QUATF_EULER_LOCK 1e-3F

/* The angles (pitch about x, yaw about y, roll about z; radians) that
 * quatf_from_euler turns into `q`, a unit quaternion: pitch in [-pi/2, pi/2],
 * yaw and roll in (-pi, pi]. With R = Ry Rx Rz, R[1][2] = -sin pitch, row 1's
 * first two entries carry cos pitch and roll, and column 2 carries yaw. At
 * pitch +-90 degrees yaw and roll turn about one axis; roll is then 0 and yaw
 * takes the whole turn. */
static inline vec3f_t
quatf_to_euler(quatf_t q) {
    const float r00 = 1.0F - (2.0F * ((q.y * q.y) + (q.z * q.z)));
    const float r02 = 2.0F * ((q.x * q.z) + (q.w * q.y));
    const float r10 = 2.0F * ((q.x * q.y) + (q.w * q.z));
    const float r11 = 1.0F - (2.0F * ((q.x * q.x) + (q.z * q.z)));
    const float r12 = 2.0F * ((q.y * q.z) - (q.w * q.x));
    const float r20 = 2.0F * ((q.x * q.z) - (q.w * q.y));
    const float r22 = 1.0F - (2.0F * ((q.x * q.x) + (q.y * q.y)));
    const float cos_pitch = sqrtf((r10 * r10) + (r11 * r11));
    const float pitch = atan2f(-r12, cos_pitch);
    if (cos_pitch < QUATF_EULER_LOCK) {
        return (vec3f_t){pitch, atan2f(-r20, r00), 0.0F};
    }
    return (vec3f_t){pitch, atan2f(r02, r22), atan2f(r10, r11)};
}
