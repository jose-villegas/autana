/*
 * anim_track: keyed values over time, sampled the way glTF 2.0 defines
 * them, so a track authored in Blender and baked by tools/anim/ plays back
 * unchanged. A track knows nothing of what it drives: a caller maps the
 * sampled numbers onto a camera, a light, a material value, anything a
 * scene exposes. A node's translation, rotation and scale are three tracks,
 * and a property reached by a glTF animation pointer is one more.
 *
 * Single precision only, and no allocation: a track points at const arrays,
 * usually the ones the baker wrote. Portable and ESP-IDF-free.
 */
#pragma once

#include <stdint.h>

#define ANIM_WIDTH_MAX 4

typedef enum {
    ANIM_STEP,   /* holds a key's value until the next key */
    ANIM_LINEAR, /* lerp, or slerp for a quaternion track */
    ANIM_CUBIC,  /* glTF CUBICSPLINE: an in-tangent, value and out-tangent per key */
} anim_interp_t;

typedef enum {
    ANIM_CLAMP, /* before the start and after the end hold the end values */
    ANIM_LOOP,  /* time wraps at the clip's duration */
} anim_wrap_t;

typedef struct {
    const float* times;  /* seconds, strictly increasing */
    const float* values; /* `width` floats per key, or three such runs per key when cubic */
    uint16_t count;      /* keys, at least 1 */
    uint8_t width;       /* 1 to ANIM_WIDTH_MAX */
    uint8_t interp;      /* anim_interp_t */
    uint8_t quaternion;  /* nonzero: xyzw, interpolated as a rotation */
} anim_track_t;

/* The timeline glTF plays all of one animation's tracks on: a track's keys
 * are in clip seconds, so tracks starting or ending at different times stay
 * in step. */
typedef struct {
    uint32_t duration_ms; /* the last key of any track */
} anim_clip_t;

/* Clip seconds for `t_ms`: wrapped by integer milliseconds so a long run
 * keeps its resolution, and held at the duration when clamped. */
float anim_clip_seconds(const anim_clip_t* clip, uint32_t t_ms, anim_wrap_t wrap);

/* Writes `width` floats. Before the first key and after the last, the end
 * value, as glTF defines. */
void anim_track_sample(const anim_track_t* track, float seconds, float out[ANIM_WIDTH_MAX]);

/* Rotates v by the unit quaternion q (xyzw). */
void anim_quat_rotate(const float q[4], const float v[3], float out[3]);
