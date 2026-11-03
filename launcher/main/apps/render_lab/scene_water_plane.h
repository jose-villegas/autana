/*
 * scene_water_plane: the water scene's cheap option to flip3 (water.plane).
 * No particles: the surface is a plane through the box at the fill's
 * volume, its up swinging toward the push's up as a damped pendulum (the
 * first sloshing mode's equivalent pendulum), so it has no crests or
 * splash. The plane is written as a density field over flip3_field()'s
 * samples, so the scene meshes, shades and builds it as it does flip3's.
 */
#pragma once

#include "math/geometry/iso_surface.h"
#include "math/linear/vec3f.h"

typedef struct {
    vec3f_t up;   /* box axes (y is down) */
    vec3f_t rate; /* the up's rate of change, per second */
    float level;  /* the plane's height along up, mm */
} water_plane_t;

/* Level, at rest, through the middle of a box of size `box`. */
void water_plane_reset(water_plane_t* p, vec3f_t box);

/* The swing toward the push's up over `dt_s`, at natural frequency `hz` and
 * `damping` of critical. */
void water_plane_step(water_plane_t* p, float dt_s, vec3f_t accel_g, float hz, float damping);

/* Writes the plane into `field`'s inside samples, its level found for the
 * `fill` share of them by Newton's method from the last frame's level. */
void water_plane_field(water_plane_t* p, const iso_field_t* field, float fill);
