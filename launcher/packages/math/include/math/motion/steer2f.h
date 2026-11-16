/*
 * steer2f: steering behaviours for agents on a ground plane (Reynolds 1999):
 * arrive at a point, keep apart from neighbours, and integrate one fixed
 * step with an acceleration, speed and turn-rate limit. Plain maths: an agent
 * is a position, a velocity and a heading; nothing here knows a scene.
 */
#pragma once

#include "math/linear/vec2f.h"

typedef struct {
    vec2f_t position; /* metres on the ground plane */
    vec2f_t velocity; /* metres per second */
    float heading;    /* radians from +x toward +y, in [-pi, pi] */
} steer2f_agent_t;

typedef struct {
    float max_speed;         /* metres per second */
    float max_accel;         /* metres per second squared; every steer is clamped to it */
    float turn_rate;         /* radians per second the heading may turn */
    float arrive_radius;     /* metres from the target where arrive starts to slow */
    float separation_radius; /* metres: neighbours nearer than this push apart */
    float heading_speed;     /* metres per second below which the heading holds */
    float horizon_s;         /* seconds ahead avoid looks for a near miss */
} steer2f_limits_t;

/* The acceleration that brings `a` to `target` at `speed`, slowing linearly
 * inside arrive_radius to stop on it; at most max_accel long. */
vec2f_t steer2f_arrive(const steer2f_agent_t* a, vec2f_t target, float speed, const steer2f_limits_t* l);

/* The acceleration pushing agents[self] away from every other agent nearer
 * than separation_radius, stronger the nearer it is; at most max_accel long,
 * zero with no neighbour in range. */
vec2f_t steer2f_separation(const steer2f_agent_t* agents, int count, int self, const steer2f_limits_t* l);

/* The acceleration steering agents[self] off every near miss within
 * horizon_s: for each neighbour, the time their paths come closest, and if
 * they would pass nearer than separation_radius, a push apart along their
 * offset at that time, stronger the nearer and the sooner; at most max_accel
 * long (Reynolds's unaligned collision avoidance). */
vec2f_t steer2f_avoid(const steer2f_agent_t* agents, int count, int self, const steer2f_limits_t* l);

/* The acceleration turning `a` back inside the circle of `radius` round
 * `centre`: zero inside it, toward the centre outside, growing to max_accel
 * at arrive_radius past the edge. */
vec2f_t steer2f_contain(const steer2f_agent_t* a, vec2f_t centre, float radius, const steer2f_limits_t* l);

/* One step of dt seconds: the acceleration clamped to max_accel, the speed to
 * max_speed, then the position moved; the heading turns toward the velocity by
 * at most turn_rate * dt, and holds below heading_speed. */
void steer2f_step(steer2f_agent_t* a, vec2f_t accel, float dt, const steer2f_limits_t* l);
