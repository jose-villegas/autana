/*
 * motion_profile: a move along a line as a function of time, an S-curve
 * motion profile. It rests at its start, eases up to its cruise speed, holds
 * it, eases down to rest at its end and rests again. Each ramp's speed is a
 * smoothstep of its time, so speed and place have no jumps. Nothing is
 * integrated: any time is one call.
 */
#pragma once

typedef struct {
    float start, end; /* along the line; end > start */
    float peak_speed; /* units per second the cruise holds at most */
    float rest_s;     /* still, at each end */
    float ramp_s;     /* easing up, and again easing down */
} motion_profile_t;

typedef struct {
    float at;    /* along the line */
    float speed; /* units per second, never below 0 */
} motion_profile_at_t;

/* The speed the cruise holds: the peak, or lower when two ramps at the peak
 * would already overrun the distance (then there is no cruise). */
float motion_profile_cruise_speed(const motion_profile_t* p);

/* From the first rest's start to the last rest's end. */
float motion_profile_duration_s(const motion_profile_t* p);

/* Where the move is, and how fast, `t_s` from its start; held at its start
 * before 0 and at its end from motion_profile_duration_s() on. */
motion_profile_at_t motion_profile_at(const motion_profile_t* p, float t_s);
