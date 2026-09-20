/*
 * rt_cornell - see rt_cornell.h. Geometry is adapted from the classic
 * Cornell Box reference arrangement (white room, red left / green right,
 * a ceiling light, a short and a tall box turned oppositely), rebuilt at a
 * normalized scale rather than copied from any one file.
 *
 * float only, everywhere: the S3's FPU has no double, so a stray one falls
 * into software emulation an order of magnitude slower. Both pragmas below
 * make that a compile error rather than a hope.
 */
#pragma GCC diagnostic error "-Wdouble-promotion"
#pragma GCC diagnostic error "-Wfloat-conversion"

#include "rt_cornell.h"

#include <math.h>

#define RT_EPSILON                0.0001f
#define RT_MAX_T                  100.0f

#define ROOM_HALF_X               1.0f
#define ROOM_HEIGHT               2.0f
#define ROOM_DEPTH                2.0f

#define LIGHT_HALF_X              0.24f
#define LIGHT_HALF_Z              0.24f
#define LIGHT_CENTER_Z            1.0f
#define LIGHT_POS                 ((r3d_vec3f_t){0.0f, ROOM_HEIGHT - 0.05f, LIGHT_CENTER_Z})
#define LIGHT_EMISSIVE_RGB        0xFFF6E0u

#define WALL_WHITE                ((r3d_vec3f_t){0.76f, 0.75f, 0.74f})
#define WALL_RED                  ((r3d_vec3f_t){0.63f, 0.065f, 0.05f})
#define WALL_GREEN                ((r3d_vec3f_t){0.14f, 0.45f, 0.091f})
#define BOX_ALBEDO                ((r3d_vec3f_t){0.78f, 0.78f, 0.75f})

/* sin/cos of 17 degrees, computed once here rather than by a trig call on
 * every ray/box test. */
#define BOX_YAW_SIN               0.29237170472f
#define BOX_YAW_COS               0.95630475596f

#define AMBIENT                   0.26f
#define LIGHT_INTENSITY           2.0f
#define SHADOW_BIAS               0.001f

#define CAMERA_POS                ((r3d_vec3f_t){0.0f, 1.0f, -2.6f})
#define CAMERA_FORWARD            ((r3d_vec3f_t){0.0f, 0.0f, 1.0f})
#define CAMERA_RIGHT              ((r3d_vec3f_t){1.0f, 0.0f, 0.0f}) /* +X is the green wall's side */
#define CAMERA_UP                 ((r3d_vec3f_t){0.0f, 1.0f, 0.0f})
/* The room's open front, 1 unit either side of the axis, just fits the
 * screen's SHORTER axis from CAMERA_POS, so neither box is ever cropped; the
 * longer axis sees a little past the room. */
#define CAMERA_HALF_FOV_SHORT_TAN (1.04f / 2.6f)

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

/* An axis-aligned rectangle bounded to a finite span, rather than an
 * infinite plane: every room surface and the ceiling light are one of
 * these. `d` is the plane's offset (dot(point, normal) == d); the bounds
 * are on whichever two axes `normal` is not aligned with, in ascending
 * axis order. */
typedef struct {
    r3d_vec3f_t normal;
    float d;
    float min1, max1, min2, max2;
    r3d_vec3f_t albedo;
} rt_wall_t;

static const rt_wall_t light_quad = {
    {0.0f, -1.0f, 0.0f},           -ROOM_HEIGHT,       -LIGHT_HALF_X, LIGHT_HALF_X, LIGHT_CENTER_Z - LIGHT_HALF_Z,
    LIGHT_CENTER_Z + LIGHT_HALF_Z, {0.0f, 0.0f, 0.0f},
};

static const rt_wall_t walls[] = {
    {{0.0f, 1.0f, 0.0f}, 0.0f, -ROOM_HALF_X, ROOM_HALF_X, 0.0f, ROOM_DEPTH, WALL_WHITE},          /* floor */
    {{0.0f, -1.0f, 0.0f}, -ROOM_HEIGHT, -ROOM_HALF_X, ROOM_HALF_X, 0.0f, ROOM_DEPTH, WALL_WHITE}, /* ceiling */
    {{0.0f, 0.0f, -1.0f}, -ROOM_DEPTH, -ROOM_HALF_X, ROOM_HALF_X, 0.0f, ROOM_HEIGHT, WALL_WHITE}, /* back */
    {{1.0f, 0.0f, 0.0f}, -ROOM_HALF_X, 0.0f, ROOM_HEIGHT, 0.0f, ROOM_DEPTH, WALL_RED},            /* left */
    {{-1.0f, 0.0f, 0.0f}, -ROOM_HALF_X, 0.0f, ROOM_HEIGHT, 0.0f, ROOM_DEPTH, WALL_GREEN},         /* right */
};
#define WALL_COUNT ((int)(sizeof(walls) / sizeof(walls[0])))

static const rt_box_t boxes[] = {
    {{0.35f, 0.28f, 0.65f}, {0.28f, 0.28f, 0.28f}, -BOX_YAW_SIN, BOX_YAW_COS}, /* short, front-right */
    {{-0.35f, 0.55f, 1.3f}, {0.28f, 0.55f, 0.28f}, BOX_YAW_SIN, BOX_YAW_COS},  /* tall, back-left */
};
#define BOX_COUNT ((int)(sizeof(boxes) / sizeof(boxes[0])))

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

typedef struct {
    float t;
    r3d_vec3f_t point, normal, albedo;
    bool is_light;
} rt_hit_t;

static bool
scene_intersect(r3d_vec3f_t origin, r3d_vec3f_t dir, rt_hit_t* hit) {
    bool found = false;
    hit->t = RT_MAX_T;

    float t;
    r3d_vec3f_t n;
    if (intersect_wall(origin, dir, &light_quad, &t, &n) && t < hit->t) {
        *hit = (rt_hit_t){t, {0, 0, 0}, n, {0, 0, 0}, true};
        found = true;
    }
    for (int i = 0; i < WALL_COUNT; i++) {
        if (intersect_wall(origin, dir, &walls[i], &t, &n) && t < hit->t) {
            *hit = (rt_hit_t){t, {0, 0, 0}, n, walls[i].albedo, false};
            found = true;
        }
    }
    for (int i = 0; i < BOX_COUNT; i++) {
        r3d_vec3f_t bn;
        float bt;
        if (rt_intersect_box(origin, dir, &boxes[i], &bt, &bn) && bt < hit->t) {
            *hit = (rt_hit_t){bt, {0, 0, 0}, bn, BOX_ALBEDO, false};
            found = true;
        }
    }

    if (found) {
        hit->point = r3d_vec3f_add(origin, r3d_vec3f_scale(dir, hit->t));
    }
    return found;
}

/* Boxes only: the room's own walls bound a convex interior, so a straight
 * line between two points already inside it can never cross one. */
static bool
scene_occluded(r3d_vec3f_t origin, r3d_vec3f_t dir, float max_t) {
    for (int i = 0; i < BOX_COUNT; i++) {
        float t;
        r3d_vec3f_t n;
        if (rt_intersect_box(origin, dir, &boxes[i], &t, &n) && t < max_t) {
            return true;
        }
    }
    return false;
}

static r3d_vec3f_t
shade_point(r3d_vec3f_t point, r3d_vec3f_t normal, r3d_vec3f_t albedo) {
    const r3d_vec3f_t to_light = r3d_vec3f_sub(LIGHT_POS, point);
    const float dist = sqrtf(r3d_vec3f_dot(to_light, to_light));
    const r3d_vec3f_t light_dir = r3d_vec3f_scale(to_light, 1.0f / dist);

    float diffuse = r3d_vec3f_dot(normal, light_dir);
    if (diffuse < 0.0f) {
        diffuse = 0.0f;
    }
    if (diffuse > 0.0f) {
        const r3d_vec3f_t shadow_origin = r3d_vec3f_add(point, r3d_vec3f_scale(normal, SHADOW_BIAS));
        if (scene_occluded(shadow_origin, light_dir, dist - SHADOW_BIAS)) {
            diffuse = 0.0f;
        }
    }

    float brightness = AMBIENT + diffuse * (LIGHT_INTENSITY / (dist * dist));
    if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    return r3d_vec3f_scale(albedo, brightness);
}

/* A smooth wall crosses only a handful of 5-bit levels, which shows as
 * contour bands; an ordered threshold per pixel before truncating trades them
 * for a fixed pattern the eye averages. */
static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint32_t
quantize_channel(float v, uint32_t max_level, float threshold) {
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 1.0f) {
        return max_level;
    }
    const uint32_t level = (uint32_t)(v * (float)max_level + threshold);
    return level > max_level ? max_level : level;
}

static gfx_color_t
to_gfx_color(r3d_vec3f_t c, int x, int y) {
    const float threshold = ((float)bayer4[y & 3][x & 3] + 0.5f) / 16.0f;
    const uint32_t rgb565 = (quantize_channel(c.x, 31, threshold) << 11) | (quantize_channel(c.y, 63, threshold) << 5)
                            | quantize_channel(c.z, 31, threshold);
    return (gfx_color_t)((rgb565 >> 8) | (rgb565 << 8));
}

static gfx_color_t
trace_primary(r3d_vec3f_t origin, r3d_vec3f_t dir, int x, int y) {
    rt_hit_t hit;
    if (!scene_intersect(origin, dir, &hit)) {
        return GFX_RGB(0x000000u);
    }
    if (hit.is_light) {
        return GFX_RGB(LIGHT_EMISSIVE_RGB);
    }
    return to_gfx_color(shade_point(hit.point, hit.normal, hit.albedo), x, y);
}

void
rt_cornell_camera_init(rt_cornell_camera_t* cam, int width, int height, int quarter) {
    const r3d_viewport_t viewport = {.width = width, .height = height, .quarter = quarter};
    r3d_ray_camera_init(cam, CAMERA_POS, CAMERA_FORWARD, CAMERA_RIGHT, CAMERA_UP, CAMERA_HALF_FOV_SHORT_TAN, viewport);
}

gfx_color_t
rt_cornell_render_pixel(const rt_cornell_camera_t* cam, int x, int y) {
    return trace_primary(cam->origin, r3d_ray_direction(cam, x, y), x, y);
}

void
rt_cornell_render_row(const rt_cornell_camera_t* cam, int y, gfx_color_t* out_row) {
    for (int x = 0; x < cam->viewport.width; x++) {
        out_row[x] = rt_cornell_render_pixel(cam, x, y);
    }
}
