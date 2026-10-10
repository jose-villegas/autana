#include "anim/anim_track.h"

#include <math.h>
#include <stdbool.h>

#include "math/linear/quatf.h"

static const float*
key_value(const anim_track_t* track, int key) {
    const int stride = track->interp == ANIM_CUBIC ? 3 * track->width : track->width;
    const int offset = track->interp == ANIM_CUBIC ? track->width : 0;
    return &track->values[(key * stride) + offset];
}

static const float*
key_tangent(const anim_track_t* track, int key, bool out_tangent) {
    return key_value(track, key) + (out_tangent ? track->width : -(int)track->width);
}

static quatf_t
load_quat(const float* v) {
    return (quatf_t){v[0], v[1], v[2], v[3]};
}

static void
store_quat(quatf_t q, float* out) {
    out[0] = q.x;
    out[1] = q.y;
    out[2] = q.z;
    out[3] = q.w;
}

static void
hermite(const anim_track_t* track, int lo, float s, float dt, float* out) {
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float* p0 = key_value(track, lo);
    const float* p1 = key_value(track, lo + 1);
    const float* m0 = key_tangent(track, lo, true);
    const float* m1 = key_tangent(track, lo + 1, false);
    for (int i = 0; i < track->width; i++) {
        out[i] = (((2.0F * s3) - (3.0F * s2) + 1.0F) * p0[i]) + ((s3 - (2.0F * s2) + s) * dt * m0[i])
                 + (((-2.0F * s3) + (3.0F * s2)) * p1[i]) + ((s3 - s2) * dt * m1[i]);
    }
    if (track->quaternion) {
        store_quat(quatf_normalize(load_quat(out)), out);
    }
}

static void
copy_key(const anim_track_t* track, int key, float* out) {
    const float* v = key_value(track, key);
    for (int i = 0; i < track->width; i++) {
        out[i] = v[i];
    }
}

static void
blend(const anim_track_t* track, int lo, float seconds, float* out) {
    const float dt = track->times[lo + 1] - track->times[lo];
    const float s = (seconds - track->times[lo]) / dt;
    if (track->interp == ANIM_STEP) {
        copy_key(track, lo, out);
    } else if (track->interp == ANIM_CUBIC) {
        hermite(track, lo, s, dt, out);
    } else if (track->quaternion) {
        store_quat(quatf_slerp(load_quat(key_value(track, lo)), load_quat(key_value(track, lo + 1)), s), out);
    } else {
        const float* a = key_value(track, lo);
        const float* b = key_value(track, lo + 1);
        for (int i = 0; i < track->width; i++) {
            out[i] = a[i] + ((b[i] - a[i]) * s);
        }
    }
}

/* The last key at or before `seconds`, given seconds is inside the track. */
static int
segment_at(const anim_track_t* track, float seconds) {
    int lo = 0;
    int hi = track->count - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (track->times[mid] <= seconds) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

float
anim_clip_seconds(const anim_clip_t* clip, uint32_t t_ms, anim_wrap_t wrap) {
    if (wrap == ANIM_LOOP && clip->duration_ms > 0) {
        t_ms %= clip->duration_ms;
    } else if (t_ms > clip->duration_ms) {
        t_ms = clip->duration_ms;
    }
    return (float)t_ms * 0.001F;
}

void
anim_track_sample(const anim_track_t* track, float seconds, float out[ANIM_WIDTH_MAX]) {
    if (track->count == 1 || seconds <= track->times[0]) {
        copy_key(track, 0, out);
    } else if (seconds >= track->times[track->count - 1]) {
        copy_key(track, track->count - 1, out);
    } else {
        blend(track, segment_at(track, seconds), seconds, out);
    }
}

void
anim_quat_rotate(const float q[4], const float v[3], float out[3]) {
    const vec3f_t r = quatf_rotate(load_quat(q), (vec3f_t){v[0], v[1], v[2]});
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}
