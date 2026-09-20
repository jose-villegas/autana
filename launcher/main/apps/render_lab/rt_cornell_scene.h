/*
 * rt_cornell_scene - the Cornell box's own DATA for rt_geometry.h's tracer:
 * walls, boxes and the ceiling light, adapted from the classic Cornell Box
 * reference arrangement (white room, red left / green right, a short and a
 * tall box turned oppositely) at a normalized scale, rather than copied
 * from any one file.
 */
#pragma once

#include "render/r3d_ray.h"
#include "rt_geometry.h"

extern const rt_scene_t rt_cornell_scene;

/* The light's own position, for shading - distinct from rt_cornell_scene's
 * light quad, which is its emitting SURFACE. */
extern const r3d_vec3f_t rt_cornell_light_pos;
