/*
 * rt_cornell - see rt_cornell.h. The Whitted shading (one point light, one
 * hard shadow ray) and the pixel entry points; the tracer itself is
 * rt_geometry.c and the box's data is rt_cornell_scene.c.
 *
 * float only, everywhere: the S3's FPU has no double, so a stray one falls
 * into software emulation an order of magnitude slower. Both pragmas below
 * make that a compile error rather than a hope.
 */
#pragma GCC diagnostic error "-Wdouble-promotion"
#pragma GCC diagnostic error "-Wfloat-conversion"

#include "rt_cornell.h"

#include <math.h>

#include "rt_cornell_scene.h"
#include "rt_geometry.h"

#define LIGHT_EMISSIVE_RGB        0xFFF6E0u

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

static r3d_vec3f_t
shade_point(r3d_vec3f_t point, r3d_vec3f_t normal, r3d_vec3f_t albedo) {
    const r3d_vec3f_t to_light = r3d_vec3f_sub(rt_cornell_light_pos, point);
    const float dist = sqrtf(r3d_vec3f_dot(to_light, to_light));
    const r3d_vec3f_t light_dir = r3d_vec3f_scale(to_light, 1.0f / dist);

    float diffuse = r3d_vec3f_dot(normal, light_dir);
    if (diffuse < 0.0f) {
        diffuse = 0.0f;
    }
    if (diffuse > 0.0f) {
        const r3d_vec3f_t shadow_origin = r3d_vec3f_add(point, r3d_vec3f_scale(normal, SHADOW_BIAS));
        if (rt_scene_occluded(&rt_cornell_scene, shadow_origin, light_dir, dist - SHADOW_BIAS)) {
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

gfx_color_t
rt_cornell_dither_quantize(r3d_vec3f_t c, int x, int y) {
    const float threshold = ((float)bayer4[y & 3][x & 3] + 0.5f) / 16.0f;
    return gfx_color_rgb565((uint8_t)quantize_channel(c.x, 31, threshold),
                            (uint8_t)quantize_channel(c.y, 63, threshold),
                            (uint8_t)quantize_channel(c.z, 31, threshold));
}

static gfx_color_t
trace_primary(r3d_vec3f_t origin, r3d_vec3f_t dir, int x, int y) {
    rt_hit_t hit;
    if (!rt_scene_intersect(&rt_cornell_scene, origin, dir, &hit)) {
        return GFX_RGB(0x000000u);
    }
    if (hit.is_light) {
        return GFX_RGB(LIGHT_EMISSIVE_RGB);
    }
    return rt_cornell_dither_quantize(shade_point(hit.point, hit.normal, hit.albedo), x, y);
}

void
rt_cornell_camera_init(rt_cornell_camera_t* cam, r3d_viewport_t viewport) {
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
