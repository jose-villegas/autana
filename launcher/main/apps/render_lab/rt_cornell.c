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
#include <stddef.h>

#include "rt_cornell_scene.h"
#include "rt_geometry.h"
#include "rt_refine.h"
#include "util/job.h"

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

static void
fill_lattice_block(gfx_color_t* fb, int width, int height, int x, int y, int step, gfx_color_t color) {
    const int w = x + step > width ? width - x : step;
    const int h = y + step > height ? height - y : step;

    for (int row = 0; row < h; row++) {
        gfx_color_t* dst = fb + (size_t)(y + row) * width + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

void
rt_cornell_render_rows(const rt_cornell_camera_t* cam, gfx_color_t* fb, int y0, int y1, int step) {
    const int width = cam->viewport.width;
    const int height = cam->viewport.height;

    for (int y = y0; y < y1; y += step) {
        for (int x = 0; x < width; x += step) {
            if (!rt_refine_is_new(x, y, step)) {
                continue;
            }
            fill_lattice_block(fb, width, height, x, y, step, rt_cornell_render_pixel(cam, x, y));
        }
    }
}

typedef struct {
    const rt_cornell_camera_t* cam;
    gfx_color_t* fb;
    int y0, y1, step;
} lattice_row_job_t;

_Static_assert(sizeof(lattice_row_job_t) <= JOB_CTX_MAX, "lattice_row_job_t must fit JOB_CTX_MAX");

static void
lattice_row_job_worker(void* ctx) {
    const lattice_row_job_t* job = ctx;
    rt_cornell_render_rows(job->cam, job->fb, job->y0, job->y1, job->step);
}

int
rt_cornell_render_lattice_budget(const rt_cornell_camera_t* cam, gfx_color_t* fb, int y0, int step, int pixel_budget) {
    const int end_y = rt_refine_lattice_range_end(cam->viewport.width, cam->viewport.height, y0, step, pixel_budget);
    const int mid_y = rt_refine_split_mid(y0, end_y, step);
    const lattice_row_job_t job = {cam, fb, y0, mid_y, step};

    (void)job_run_core1(lattice_row_job_worker, &job, sizeof job);
    rt_cornell_render_rows(cam, fb, mid_y, end_y, step);
    (void)job_wait(100);
    return end_y;
}
