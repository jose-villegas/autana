/*
 * rt_geometry - see rt_geometry.h.
 *
 * float only, everywhere: the S3's FPU has no double, so a stray one falls
 * into software emulation an order of magnitude slower. Both pragmas below
 * make that a compile error rather than a hope.
 */
#pragma GCC diagnostic error "-Wdouble-promotion"
#pragma GCC diagnostic error "-Wfloat-conversion"

#include "rt_geometry.h"

#include <math.h>
#include <stddef.h>

#define RT_EPSILON 0.0001f
#define RT_MAX_T   100.0f

bool
rt_intersect_plane(r3d_vec3f_t origin, r3d_vec3f_t dir, rt_plane_t plane, float* t) {
    const float denom = r3d_vec3f_dot(dir, plane.normal);
    if (fabsf(denom) < RT_EPSILON) {
        return false;
    }
    const float candidate = r3d_vec3f_dot(r3d_vec3f_sub(plane.point, origin), plane.normal) / denom;
    if (candidate <= RT_EPSILON) {
        return false;
    }
    *t = candidate;
    return true;
}

/* Rotates `v` from world space into `box`'s own frame (v treated as a
 * direction; the caller subtracts box->center first for a position). */
static r3d_vec3f_t
box_to_local(r3d_vec3f_t v, const rt_box_t* box) {
    return (r3d_vec3f_t){
        v.x * box->cos_yaw + v.z * box->sin_yaw,
        v.y,
        -v.x * box->sin_yaw + v.z * box->cos_yaw,
    };
}

static r3d_vec3f_t
box_normal_to_world(int axis, float sign, const rt_box_t* box) {
    r3d_vec3f_t local = {0.0f, 0.0f, 0.0f};
    if (axis == 0) {
        local.x = sign;
    } else if (axis == 1) {
        local.y = sign;
    } else {
        local.z = sign;
    }
    return (r3d_vec3f_t){
        local.x * box->cos_yaw - local.z * box->sin_yaw,
        local.y,
        local.x * box->sin_yaw + local.z * box->cos_yaw,
    };
}

typedef struct {
    float t_min, t_max;
    int min_axis, max_axis;
    float min_sign, max_sign;
} rt_slab_result_t;

/* One axis of the local-frame slab test, folded into box_local_slabs()'s
 * loop rather than repeated three times by hand. */
static bool
slab_axis(int axis, float o, float d, float half, rt_slab_result_t* out) {
    if (fabsf(d) < RT_EPSILON) {
        return o >= -half && o <= half;
    }

    float near_t = (-half - o) / d;
    float far_t = (half - o) / d;
    float near_sign = -1.0f;
    float far_sign = 1.0f;
    if (near_t > far_t) {
        const float swap_t = near_t;
        near_t = far_t;
        far_t = swap_t;
        near_sign = 1.0f;
        far_sign = -1.0f;
    }

    if (near_t > out->t_min) {
        out->t_min = near_t;
        out->min_axis = axis;
        out->min_sign = near_sign;
    }
    if (far_t < out->t_max) {
        out->t_max = far_t;
        out->max_axis = axis;
        out->max_sign = far_sign;
    }
    return out->t_min <= out->t_max;
}

static bool
box_local_slabs(r3d_vec3f_t o, r3d_vec3f_t d, r3d_vec3f_t half, rt_slab_result_t* out) {
    out->t_min = -RT_MAX_T;
    out->t_max = RT_MAX_T;
    out->min_axis = -1;
    out->max_axis = -1;

    if (!slab_axis(0, o.x, d.x, half.x, out)) {
        return false;
    }
    if (!slab_axis(1, o.y, d.y, half.y, out)) {
        return false;
    }
    return slab_axis(2, o.z, d.z, half.z, out);
}

bool
rt_intersect_box(r3d_vec3f_t origin, r3d_vec3f_t dir, const rt_box_t* box, float* t_hit, r3d_vec3f_t* out_normal) {
    const r3d_vec3f_t local_origin = box_to_local(r3d_vec3f_sub(origin, box->center), box);
    const r3d_vec3f_t local_dir = box_to_local(dir, box);

    rt_slab_result_t slabs;
    if (!box_local_slabs(local_origin, local_dir, box->half_extent, &slabs)) {
        return false;
    }

    int axis;
    float sign, t;
    if (slabs.t_min > RT_EPSILON) {
        t = slabs.t_min;
        axis = slabs.min_axis;
        sign = slabs.min_sign;
    } else if (slabs.t_max > RT_EPSILON) {
        t = slabs.t_max; /* the ray started inside the box: report the exit face */
        axis = slabs.max_axis;
        sign = slabs.max_sign;
    } else {
        return false;
    }
    if (axis < 0) {
        return false; /* every axis was the fabsf(d) < RT_EPSILON branch */
    }

    *t_hit = t;
    *out_normal = box_normal_to_world(axis, sign, box);
    return true;
}

static bool
wall_bounds_ok(r3d_vec3f_t p, const rt_wall_t* w) {
    float c1, c2;
    if (w->normal.x != 0.0f) {
        c1 = p.y;
        c2 = p.z;
    } else if (w->normal.y != 0.0f) {
        c1 = p.x;
        c2 = p.z;
    } else {
        c1 = p.x;
        c2 = p.y;
    }
    return c1 >= w->min1 && c1 <= w->max1 && c2 >= w->min2 && c2 <= w->max2;
}

static bool
intersect_wall(r3d_vec3f_t origin, r3d_vec3f_t dir, const rt_wall_t* w, float* t, r3d_vec3f_t* normal) {
    const rt_plane_t plane = {r3d_vec3f_scale(w->normal, w->d), w->normal};
    float hit_t;
    if (!rt_intersect_plane(origin, dir, plane, &hit_t)) {
        return false;
    }
    if (!wall_bounds_ok(r3d_vec3f_add(origin, r3d_vec3f_scale(dir, hit_t)), w)) {
        return false;
    }
    *t = hit_t;
    *normal = w->normal;
    return true;
}

bool
rt_scene_intersect(const rt_scene_t* scene, r3d_vec3f_t origin, r3d_vec3f_t dir, rt_hit_t* hit) {
    bool found = false;
    hit->t = RT_MAX_T;

    float t;
    r3d_vec3f_t n;
    if (scene->light != NULL && intersect_wall(origin, dir, scene->light, &t, &n) && t < hit->t) {
        *hit = (rt_hit_t){t, {0, 0, 0}, n, {0, 0, 0}, true};
        found = true;
    }
    for (int i = 0; i < scene->wall_count; i++) {
        if (intersect_wall(origin, dir, &scene->walls[i], &t, &n) && t < hit->t) {
            *hit = (rt_hit_t){t, {0, 0, 0}, n, scene->walls[i].albedo, false};
            found = true;
        }
    }
    for (int i = 0; i < scene->box_count; i++) {
        r3d_vec3f_t bn;
        float bt;
        if (rt_intersect_box(origin, dir, &scene->boxes[i], &bt, &bn) && bt < hit->t) {
            *hit = (rt_hit_t){bt, {0, 0, 0}, bn, scene->box_albedo, false};
            found = true;
        }
    }

    if (found) {
        hit->point = r3d_vec3f_add(origin, r3d_vec3f_scale(dir, hit->t));
    }
    return found;
}

bool
rt_scene_occluded(const rt_scene_t* scene, r3d_vec3f_t origin, r3d_vec3f_t dir, float max_t) {
    for (int i = 0; i < scene->box_count; i++) {
        float t;
        r3d_vec3f_t n;
        if (rt_intersect_box(origin, dir, &scene->boxes[i], &t, &n) && t < max_t) {
            return true;
        }
    }
    return false;
}
