#include "render/framegen/framegen.h"

#include <string.h>

#include "core/job.h"
#include "core/memory.h"
#include "math/scalar/mathf.h"
#include "render/r3d_span.h"
#include "render/raster_internal.h"

#define DEPTH_SCALE (1.0F / (float)R3D_DEPTH_NEAREST)
/* A warped sky pixel: farther than anything drawn, yet not a hole. */
#define DEPTH_SKY   1U

bool
framegen_target_alloc(framegen_target_t* out, int width, int height) {
    const size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    *out = (framegen_target_t){memory_alloc(bytes, MEMORY_PSRAM), memory_alloc(bytes, MEMORY_PSRAM), width, height};
    if (out->colour == NULL || out->depth == NULL) {
        framegen_target_free(out);
        return false;
    }
    return true;
}

void
framegen_target_free(framegen_target_t* target) {
    memory_free(target->colour);
    memory_free(target->depth);
    *target = (framegen_target_t){0};
}

/* One core's share of a warp: source rows [first, end) into the whole target. */
typedef struct {
    const uint16_t* colour;
    const uint16_t* depth;
    framegen_target_t* out;
    int first, end;
    const mat4f_t* map;
    projectf_t project;
} warp_rows_t;

/* One lens-space axis of the map along a row, the row's v folded in:
 * the axis is du u + base + dw w. */
typedef struct {
    float du, base, dw;
} lens_axis_t;

static inline float
axis_at(lens_axis_t a, float u, float w) {
    return (a.du * u) + a.base + (a.dw * w);
}

/* The warped depth word: the sky stays the farthest, anything drawn sits above it. */
static inline uint32_t
moved_depth(uint16_t depth, float moved) {
    if (depth == R3D_DEPTH_EMPTY) {
        return DEPTH_SKY;
    }
    const float nearest = (float)R3D_DEPTH_NEAREST;
    const float above_sky = (float)(DEPTH_SKY + 1U);
    return (uint32_t)(moved >= nearest ? nearest : moved < above_sky ? above_sky : moved);
}

/* Lens space is pixels at unit depth: depth word d is the point (u, v, 1)
 * near_z / iz, and through the map its new inverse depth is d / z. The sky
 * (d = 0) has w = 0, so it only turns. to_w turns a depth word into w. */
static inline void
splat(const warp_rows_t* job, const lens_axis_t axes[3], float to_w, float u, uint16_t depth, uint16_t colour) {
    const framegen_target_t* out = job->out;
    const float d = (float)depth;
    const float w = d * to_w;
    const float z = axis_at(axes[2], u, w);
    if (!(z > 0.0F)) {
        return;
    }
    const float inv = mathf_recip(z);
    const float px = (axis_at(axes[0], u, w) * inv) + job->project.cx;
    const float py = (axis_at(axes[1], u, w) * inv) + job->project.cy;
    if (!(px >= 0.0F && py >= 0.0F && px < (float)out->width && py < (float)out->height)) {
        return;
    }
    const size_t at = ((size_t)py * (size_t)out->width) + (size_t)px;
    const uint32_t nd = moved_depth(depth, d * inv);
    if (nd > out->depth[at]) {
        out->depth[at] = (uint16_t)nd;
        out->colour[at] = colour;
    }
}

static void
warp_rows(void* ctx) {
    const warp_rows_t* job = ctx;
    const float to_w = DEPTH_SCALE / job->project.near_z;
    const float (*m)[4] = job->map->m;
    const int width = job->out->width;
    for (int y = job->first; y < job->end; y++) {
        const float v = (float)y + 0.5F - job->project.cy;
        const lens_axis_t axes[3] = {
            {m[0][0], (m[0][1] * v) + m[0][2], m[0][3]},
            {m[1][0], (m[1][1] * v) + m[1][2], m[1][3]},
            {m[2][0], (m[2][1] * v) + m[2][2], m[2][3]},
        };
        const size_t row = (size_t)y * (size_t)width;
        float u = 0.5F - job->project.cx;
        for (int x = 0; x < width; x++, u += 1.0F) {
            splat(job, axes, to_w, u, job->depth[row + (size_t)x], job->colour[row + (size_t)x]);
        }
    }
}

void
framegen_warp(const raster_t* raster, const render_view_t* drawn, const render_view_t* now, framegen_target_t* out) {
    r3d_lens_t before;
    r3d_lens_t after;
    raster_lens(raster, drawn, 1, &before);
    raster_lens(raster, now, 1, &after);
    const mat4f_t map = mat4f_mul_affine(after.m, mat4f_invert_affine(before.m));
    memset(out->depth, 0, (size_t)out->width * (size_t)out->height * sizeof(uint16_t));
    /* Two rows may race for one target pixel; either splat is a fair pick. */
    warp_rows_t first = {raster_color(raster), raster_depth(raster), out, 0, out->height / 2, &map, before.project};
    warp_rows_t second = first;
    second.first = first.end;
    second.end = out->height;
    (void)job_run_split(warp_rows, &first, &second, sizeof second);
}

/* One core's share of the fill: pixels [first, end). */
typedef struct {
    uint16_t* colour;
    uint16_t* depth;
    const framegen_target_t* target;
    size_t first, end;
    int* holes; /* the caller's: core 1 runs on a copy of this context */
} fill_span_t;

static void
fill_span(void* ctx) {
    const fill_span_t* job = ctx;
    int holes = 0;
    for (size_t i = job->first; i < job->end; i++) {
        const uint16_t d = job->target->depth[i];
        if (d == R3D_DEPTH_EMPTY) {
            holes++;
            continue;
        }
        job->colour[i] = job->target->colour[i];
        job->depth[i] = d == DEPTH_SKY ? R3D_DEPTH_EMPTY : d;
    }
    *job->holes = holes;
}

int
framegen_fill(const raster_t* raster, const framegen_target_t* target) {
    const size_t pixels = (size_t)target->width * (size_t)target->height;
    int holes[2] = {0, 0};
    fill_span_t first = {raster_color(raster), raster_depth(raster), target, 0, pixels / 2, &holes[0]};
    fill_span_t second = first;
    second.first = first.end;
    second.end = pixels;
    second.holes = &holes[1];
    (void)job_run_split(fill_span, &first, &second, sizeof second);
    return holes[0] + holes[1];
}
