/*
 * rt_geometry - the small general tracer a Whitted-style integrator sits on
 * top of: ray/plane, ray/rotated-box, a scene of const walls and boxes plus
 * one light, nearest hit and occlusion. No scene DATA lives here - a scene
 * is built and owned by whoever traces it, such as rt_cornell_scene.c.
 *
 * ESP-IDF-free and host-testable, on r3d_vec3f_t (render/r3d_ray.h).
 */
#pragma once

#include <stdbool.h>

#include "render/r3d_ray.h"

/* An infinite plane: `point` is any point on it, `normal` is unit length. */
typedef struct {
    r3d_vec3f_t point;
    r3d_vec3f_t normal;
} rt_plane_t;

/* Hit distance for a ray against `plane`, ahead of `origin`. False for a
 * miss: parallel to the plane, or the crossing is behind the ray. */
bool rt_intersect_plane(r3d_vec3f_t origin, r3d_vec3f_t dir, rt_plane_t plane, float* t);

/* An oriented box: axis-aligned in its own frame, turned `sin_yaw`/`cos_yaw`
 * about the vertical (Y) axis in world space. */
typedef struct {
    r3d_vec3f_t center;
    r3d_vec3f_t half_extent;
    float sin_yaw, cos_yaw;
} rt_box_t;

/* Hit distance and outward world-space normal for a ray against `box`,
 * rotating the ray into the box's own frame and running a slab test there.
 * Reports the exit face rather than the entry when the ray starts inside.
 * False for a miss. */
bool rt_intersect_box(r3d_vec3f_t origin, r3d_vec3f_t dir, const rt_box_t* box, float* t_hit, r3d_vec3f_t* out_normal);

/* An axis-aligned rectangle bounded to a finite span, rather than an
 * infinite plane: a room surface or a ceiling light is one of these. `d` is
 * the plane's offset (dot(point, normal) == d); the bounds are on whichever
 * two axes `normal` is not aligned with, in ascending axis order. */
typedef struct {
    r3d_vec3f_t normal;
    float d;
    float min1, max1, min2, max2;
    r3d_vec3f_t albedo;
} rt_wall_t;

/* One ray's nearest hit: distance, point, normal, albedo, and whether it
 * struck the scene's own light rather than a surface. */
typedef struct {
    float t;
    r3d_vec3f_t point, normal, albedo;
    bool is_light;
} rt_hit_t;

/* A scene an integrator traces against: one emitting wall (`light`, its own
 * albedo unused), an array of surface walls, and an array of boxes sharing
 * `box_albedo` - this tracer has no per-box material yet. */
typedef struct {
    const rt_wall_t* light;
    const rt_wall_t* walls;
    int wall_count;
    const rt_box_t* boxes;
    int box_count;
    r3d_vec3f_t box_albedo;
} rt_scene_t;

/* The nearest thing `dir` from `origin` hits in `scene` - the light first,
 * then every wall, then every box. False when nothing is hit. */
bool rt_scene_intersect(const rt_scene_t* scene, r3d_vec3f_t origin, r3d_vec3f_t dir, rt_hit_t* hit);

/* Boxes only: true only for a scene whose walls bound a convex interior, so
 * a straight line between two points already inside it can never cross
 * one. */
bool rt_scene_occluded(const rt_scene_t* scene, r3d_vec3f_t origin, r3d_vec3f_t dir, float max_t);
