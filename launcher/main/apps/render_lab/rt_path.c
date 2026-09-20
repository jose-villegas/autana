/*
 * rt_path - see rt_path.h.
 *
 * float only, everywhere: the S3's FPU has no double, so a stray one falls
 * into software emulation an order of magnitude slower. Both pragmas below
 * make that a compile error rather than a hope.
 */
#pragma GCC diagnostic error "-Wdouble-promotion"
#pragma GCC diagnostic error "-Wfloat-conversion"

#include "rt_path.h"

#include <math.h>
#include <stddef.h>

#include "rt_cornell_scene.h"
#include "rt_geometry.h"
#include "rt_refine.h"
#include "util/job.h"

#define RT_PATH_MAX_DEPTH   3
#define RT_PATH_SHADOW_BIAS 0.001f
#define RT_PATH_PI          3.14159265f

/* Warm white, the same hue rt_cornell.c's LIGHT_EMISSIVE_RGB shows for a
 * direct look at the fixture; the magnitude is a path tracer's own, chosen
 * against the rendered picture rather than the display colour Whitted
 * shading uses, since the two integrators are not comparable quantities. */
static const r3d_vec3f_t LIGHT_COLOR = {1.0f, 0.965f, 0.878f};
#define LIGHT_RADIANCE    50.0f

#define ACCUM_FIXED_SHIFT 6
#define ACCUM_FIXED_SCALE (1 << ACCUM_FIXED_SHIFT) /* 64: 1/64 resolution, up to ~1023.98 */
#define ACCUM_FIXED_MAX   0xFFFFu

/* Vector helpers rt_path.c alone needs - r3d_ray.h stays the shared
 * add/sub/scale/dot/normalize set every ray caller uses. */

static r3d_vec3f_t
vec3_mul(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.x * b.x, a.y * b.y, a.z * b.z};
}

static r3d_vec3f_t
vec3_cross(r3d_vec3f_t a, r3d_vec3f_t b) {
    return (r3d_vec3f_t){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/* Fixed-point accumulator */

static uint16_t
fixed_from_float(float v) {
    if (v <= 0.0f) {
        return 0;
    }
    const float scaled = v * (float)ACCUM_FIXED_SCALE + 0.5f;
    if (scaled >= (float)ACCUM_FIXED_MAX) {
        return ACCUM_FIXED_MAX;
    }
    return (uint16_t)scaled;
}

static float
fixed_to_float(uint16_t fixed) {
    return (float)fixed * (1.0f / (float)ACCUM_FIXED_SCALE);
}

static uint16_t
accum_channel(uint16_t mean_fixed, float sample, uint32_t n) {
    const int32_t sample_fixed = (int32_t)fixed_from_float(sample);
    const int32_t delta = sample_fixed - (int32_t)mean_fixed;
    const int32_t step = n != 0 ? delta / (int32_t)n : delta;
    int32_t result = (int32_t)mean_fixed + step;

    if (result < 0) {
        result = 0;
    } else if (result > (int32_t)ACCUM_FIXED_MAX) {
        result = (int32_t)ACCUM_FIXED_MAX;
    }
    return (uint16_t)result;
}

void
rt_path_accum_add(rt_path_accum_px_t* px, r3d_vec3f_t sample, uint32_t n) {
    px->r = accum_channel(px->r, sample.x, n);
    px->g = accum_channel(px->g, sample.y, n);
    px->b = accum_channel(px->b, sample.z, n);
}

r3d_vec3f_t
rt_path_accum_radiance(rt_path_accum_px_t px) {
    return (r3d_vec3f_t){fixed_to_float(px.r), fixed_to_float(px.g), fixed_to_float(px.b)};
}

/* Resolve */

/* A plain Reinhard curve (c / (1 + c)) compresses every channel, not only
 * the ones over 1.0 - measured against this scene it read visibly darker
 * than the Whitted picture at ordinary, already-under-1.0 brightness. Below
 * the knee a channel passes through unchanged; above it, the same Reinhard
 * shape takes over so a highlight still softens instead of clipping flat. */
#define RT_PATH_TONEMAP_KNEE 0.9f

static float
tonemap_channel(float c) {
    if (c <= RT_PATH_TONEMAP_KNEE) {
        return c;
    }
    const float span = 1.0f - RT_PATH_TONEMAP_KNEE;
    const float over = c - RT_PATH_TONEMAP_KNEE;
    return RT_PATH_TONEMAP_KNEE + span * over / (over + span);
}

r3d_vec3f_t
rt_path_tonemap(r3d_vec3f_t linear) {
    return (r3d_vec3f_t){tonemap_channel(linear.x), tonemap_channel(linear.y), tonemap_channel(linear.z)};
}

gfx_color_t
rt_path_resolve_radiance(r3d_vec3f_t radiance, int x, int y) {
    const r3d_vec3f_t clamped = {
        radiance.x < 0.0f ? 0.0f : radiance.x,
        radiance.y < 0.0f ? 0.0f : radiance.y,
        radiance.z < 0.0f ? 0.0f : radiance.z,
    };
    return rt_cornell_dither_quantize(rt_path_tonemap(clamped), x, y);
}

gfx_color_t
rt_path_resolve(rt_path_accum_px_t px, int x, int y) {
    return rt_path_resolve_radiance(rt_path_accum_radiance(px), x, y);
}

/* Sampler - hash-seeded, no per-pixel state buffer: the generator's whole
 * state is folded from (x, y, sample_index) at the start of every call. */

typedef struct {
    uint32_t state;
} rt_path_rng_t;

static uint32_t
hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static rt_path_rng_t
rng_init(int x, int y, uint32_t sample_index) {
    const uint32_t seed = ((uint32_t)x * 1973u) ^ ((uint32_t)y * 9277u) ^ (sample_index * 26699u) ^ 0x9e3779b9u;
    return (rt_path_rng_t){hash_u32(seed)};
}

static float
rng_next(rt_path_rng_t* rng) {
    rng->state = hash_u32(rng->state);
    return (float)(rng->state >> 8) * (1.0f / 16777216.0f); /* 24 bits, [0, 1) */
}

/* Light quad */

/* A point on wall `w` for (u, v) in [0, 1) each, mirroring
 * wall_bounds_ok()'s (rt_geometry.c) own axis convention: the two bounded
 * axes are whichever `normal` is not aligned with, in ascending order. */
static r3d_vec3f_t
wall_point_for_uv(const rt_wall_t* w, float u, float v) {
    const float c1 = w->min1 + u * (w->max1 - w->min1);
    const float c2 = w->min2 + v * (w->max2 - w->min2);

    if (w->normal.x != 0.0f) {
        return (r3d_vec3f_t){w->d / w->normal.x, c1, c2};
    }
    if (w->normal.y != 0.0f) {
        return (r3d_vec3f_t){c1, w->d / w->normal.y, c2};
    }
    return (r3d_vec3f_t){c1, c2, w->d / w->normal.z};
}

static float
wall_area(const rt_wall_t* w) {
    return (w->max1 - w->min1) * (w->max2 - w->min2);
}

/* The light quad's own centre - the seed pass's sample point: no RNG, so a
 * flat lit surface reads as one continuous falloff rather than a per-pixel
 * dice roll. */
static r3d_vec3f_t
light_center(void) {
    return wall_point_for_uv(rt_cornell_scene.light, 0.5f, 0.5f);
}

/* Next-event estimation against one already-chosen point on the light quad
 * - the area-light geometry term, weighted by the surface's own Lambert
 * BRDF. Zero when the light is behind either surface or the ray is
 * blocked - never a negative contribution. Shared by both callers below:
 * a full path sample and the seed pass differ only in which point on the
 * quad they hand in, never in this maths. */
static r3d_vec3f_t
direct_light_from_point(r3d_vec3f_t light_point, r3d_vec3f_t point, r3d_vec3f_t normal, r3d_vec3f_t albedo) {
    const rt_wall_t* light = rt_cornell_scene.light;
    const r3d_vec3f_t to_light = r3d_vec3f_sub(light_point, point);
    const float dist_sq = r3d_vec3f_dot(to_light, to_light);
    const float dist = sqrtf(dist_sq);
    const r3d_vec3f_t light_dir = r3d_vec3f_scale(to_light, 1.0f / dist);

    const float cos_surface = r3d_vec3f_dot(normal, light_dir);
    const float cos_light = -r3d_vec3f_dot(light->normal, light_dir);
    if (cos_surface <= 0.0f || cos_light <= 0.0f) {
        return (r3d_vec3f_t){0.0f, 0.0f, 0.0f};
    }

    const r3d_vec3f_t shadow_origin = r3d_vec3f_add(point, r3d_vec3f_scale(normal, RT_PATH_SHADOW_BIAS));
    if (rt_scene_occluded(&rt_cornell_scene, shadow_origin, light_dir, dist - RT_PATH_SHADOW_BIAS)) {
        return (r3d_vec3f_t){0.0f, 0.0f, 0.0f};
    }

    /* Lambert (albedo / pi) times the area-light estimator for uniform-area
     * sampling (pdf = 1 / area): the area and the pi cancel algebraically
     * into area / (pi * dist^2), left as three multiplies and one divide
     * rather than folded further, so each factor stays legible on its own. */
    const float geometry = cos_surface * cos_light * wall_area(light) / dist_sq;
    const float weight = geometry / RT_PATH_PI;
    const r3d_vec3f_t emission = r3d_vec3f_scale(LIGHT_COLOR, LIGHT_RADIANCE);
    return r3d_vec3f_scale(vec3_mul(albedo, emission), weight);
}

/* A uniformly sampled point on the light quad each call - what a full path
 * sample's own NEE term uses, so the shadow softens and colour bleed
 * arrives as the accumulator averages many of these together. */
static r3d_vec3f_t
direct_light(rt_path_rng_t* rng, r3d_vec3f_t point, r3d_vec3f_t normal, r3d_vec3f_t albedo) {
    const float u = rng_next(rng);
    const float v = rng_next(rng);
    const r3d_vec3f_t light_point = wall_point_for_uv(rt_cornell_scene.light, u, v);
    return direct_light_from_point(light_point, point, normal, albedo);
}

/* Cosine-weighted hemisphere bounce */

static void
build_basis(r3d_vec3f_t n, r3d_vec3f_t* tangent, r3d_vec3f_t* bitangent) {
    const r3d_vec3f_t up = fabsf(n.y) < 0.999f ? (r3d_vec3f_t){0.0f, 1.0f, 0.0f} : (r3d_vec3f_t){1.0f, 0.0f, 0.0f};
    *tangent = r3d_vec3f_normalize(vec3_cross(up, n));
    *bitangent = vec3_cross(n, *tangent);
}

/* pdf = cos(theta) / pi, the same as the Lambert BRDF's own cos/pi factor,
 * so a bounce's throughput multiplies by albedo alone - the two cancel. */
static r3d_vec3f_t
cosine_sample_hemisphere(r3d_vec3f_t n, float u1, float u2) {
    r3d_vec3f_t tangent, bitangent;
    build_basis(n, &tangent, &bitangent);

    const float r = sqrtf(u1);
    const float phi = 2.0f * RT_PATH_PI * u2;
    const float x = r * cosf(phi);
    const float y = r * sinf(phi);
    const float z = sqrtf(1.0f - u1);

    r3d_vec3f_t dir = r3d_vec3f_scale(tangent, x);
    dir = r3d_vec3f_add(dir, r3d_vec3f_scale(bitangent, y));
    dir = r3d_vec3f_add(dir, r3d_vec3f_scale(n, z));
    return dir;
}

/* Integrator */

/* `max_depth` is 1 for the direct-only reference a bleed test compares a
 * full sample against, RT_PATH_MAX_DEPTH otherwise - both walk the same
 * loop, so there is only one place the shading maths can drift. */
static r3d_vec3f_t
trace_path(r3d_vec3f_t origin, r3d_vec3f_t dir, rt_path_rng_t* rng, int max_depth) {
    r3d_vec3f_t radiance = {0.0f, 0.0f, 0.0f};
    r3d_vec3f_t throughput = {1.0f, 1.0f, 1.0f};

    for (int depth = 0; depth < max_depth; depth++) {
        rt_hit_t hit;
        if (!rt_scene_intersect(&rt_cornell_scene, origin, dir, &hit)) {
            break;
        }
        if (hit.is_light) {
            /* Only the camera ray's own hit counts the light directly - a
             * bounce landing back on it is already priced in by the NEE
             * term the previous hit took, and adding it again here would
             * double it. */
            if (depth == 0) {
                const r3d_vec3f_t emission = r3d_vec3f_scale(LIGHT_COLOR, LIGHT_RADIANCE);
                radiance = r3d_vec3f_add(radiance, vec3_mul(throughput, emission));
            }
            break;
        }

        radiance = r3d_vec3f_add(radiance, vec3_mul(throughput, direct_light(rng, hit.point, hit.normal, hit.albedo)));

        const float u1 = rng_next(rng);
        const float u2 = rng_next(rng);
        dir = cosine_sample_hemisphere(hit.normal, u1, u2);
        throughput = vec3_mul(throughput, hit.albedo);
        origin = r3d_vec3f_add(hit.point, r3d_vec3f_scale(hit.normal, RT_PATH_SHADOW_BIAS));
    }
    return radiance;
}

r3d_vec3f_t
rt_path_direct_estimate(const rt_cornell_camera_t* cam, int x, int y) {
    const r3d_vec3f_t dir = r3d_ray_direction(cam, x, y);

    rt_hit_t hit;
    if (!rt_scene_intersect(&rt_cornell_scene, cam->origin, dir, &hit)) {
        return (r3d_vec3f_t){0.0f, 0.0f, 0.0f};
    }
    if (hit.is_light) {
        return r3d_vec3f_scale(LIGHT_COLOR, LIGHT_RADIANCE);
    }
    return direct_light_from_point(light_center(), hit.point, hit.normal, hit.albedo);
}

r3d_vec3f_t
rt_path_sample(const rt_cornell_camera_t* cam, int x, int y, uint32_t sample_index) {
    rt_path_rng_t rng = rng_init(x, y, sample_index);
    const r3d_vec3f_t dir = r3d_ray_direction(cam, x, y);
    return trace_path(cam->origin, dir, &rng, RT_PATH_MAX_DEPTH);
}

r3d_vec3f_t
rt_path_sample_direct_only(const rt_cornell_camera_t* cam, int x, int y, uint32_t sample_index) {
    rt_path_rng_t rng = rng_init(x, y, sample_index);
    const r3d_vec3f_t dir = r3d_ray_direction(cam, x, y);
    return trace_path(cam->origin, dir, &rng, 1);
}

/* Progressive schedule */

static void
fill_block(gfx_color_t* fb, int width, int height, int x, int y, int step, gfx_color_t color) {
    const int w = x + step > width ? width - x : step;
    const int h = y + step > height ? height - y : step;

    for (int row = 0; row < h; row++) {
        gfx_color_t* dst = fb + (size_t)(y + row) * width + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

static gfx_color_t
seed_pixel(const rt_cornell_camera_t* cam, rt_path_accum_px_t* accum, int width, int x, int y) {
    const r3d_vec3f_t direct = rt_path_direct_estimate(cam, x, y);
    if (accum == NULL) {
        return rt_path_resolve_radiance(direct, x, y);
    }
    rt_path_accum_px_t* px = &accum[(size_t)y * width + x];
    rt_path_accum_add(px, direct, 1);
    return rt_path_resolve(*px, x, y);
}

static void
seed_lattice_row(const rt_cornell_camera_t* cam, rt_path_target_t target, int y, int step) {
    for (int x = 0; x < target.width; x += step) {
        if (!rt_refine_is_new(x, y, step)) {
            continue;
        }
        const gfx_color_t color = seed_pixel(cam, target.accum, target.width, x, y);
        fill_block(target.fb, target.width, target.height, x, y, step, color);
    }
}

void
rt_path_seed_rows(const rt_cornell_camera_t* cam, rt_path_target_t target, int y0, int y1, int step) {
    for (int y = y0; y < y1; y += step) {
        seed_lattice_row(cam, target, y, step);
    }
}

typedef struct {
    const rt_cornell_camera_t* cam;
    rt_path_target_t target;
    int y0, y1, step;
} seed_row_job_t;

_Static_assert(sizeof(seed_row_job_t) <= JOB_CTX_MAX, "seed_row_job_t must fit JOB_CTX_MAX");

static void
seed_row_job_worker(void* ctx) {
    const seed_row_job_t* job = ctx;
    rt_path_seed_rows(job->cam, job->target, job->y0, job->y1, job->step);
}

static rt_path_span_t
advance_seeding(rt_path_schedule_t* sch, const rt_cornell_camera_t* cam, rt_path_target_t target, int pixel_budget) {
    const int first_y = sch->seed_y;
    const int end_y = rt_refine_lattice_range_end(target.width, target.height, first_y, sch->step, pixel_budget);
    const int mid_y = rt_refine_split_mid(first_y, end_y, sch->step);
    const seed_row_job_t job = {cam, target, first_y, mid_y, sch->step};

    (void)job_run_core1(seed_row_job_worker, &job, sizeof job);
    rt_path_seed_rows(cam, target, mid_y, end_y, sch->step);
    (void)job_wait(100);

    sch->seed_y = end_y;
    if (sch->seed_y >= target.height) {
        sch->step = rt_refine_next_step(sch->step);
        sch->seed_y = 0;
        if (sch->step == 0) {
            sch->spp = target.accum != NULL ? 1 : 0;
        }
    }
    return (rt_path_span_t){first_y, end_y};
}

void
rt_path_sweep_rows(const rt_cornell_camera_t* cam, rt_path_target_t target, int y0, int y1, uint32_t n) {
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < target.width; x++) {
            const r3d_vec3f_t sample = rt_path_sample(cam, x, y, n);
            rt_path_accum_px_t* px = &target.accum[(size_t)y * target.width + x];
            rt_path_accum_add(px, sample, n);
            target.fb[(size_t)y * target.width + x] = rt_path_resolve(*px, x, y);
        }
    }
}

typedef struct {
    const rt_cornell_camera_t* cam;
    rt_path_target_t target;
    int y0, y1;
    uint32_t n;
} sweep_row_job_t;

_Static_assert(sizeof(sweep_row_job_t) <= JOB_CTX_MAX, "sweep_row_job_t must fit JOB_CTX_MAX");

static void
sweep_row_job_worker(void* ctx) {
    const sweep_row_job_t* job = ctx;
    rt_path_sweep_rows(job->cam, job->target, job->y0, job->y1, job->n);
}

static rt_path_span_t
advance_sweep(rt_path_schedule_t* sch, const rt_cornell_camera_t* cam, rt_path_target_t target, int pixel_budget) {
    const uint32_t n = sch->spp + 1; /* the sample this sweep folds in */
    const int first_y = sch->sweep_y;
    const int end_y = rt_refine_uniform_range_end(target.height, first_y, target.width, pixel_budget);
    const int mid_y = rt_refine_split_mid(first_y, end_y, 1);
    const sweep_row_job_t job = {cam, target, first_y, mid_y, n};

    (void)job_run_core1(sweep_row_job_worker, &job, sizeof job);
    rt_path_sweep_rows(cam, target, mid_y, end_y, n);
    (void)job_wait(100);

    if (end_y >= target.height) {
        sch->spp = n;
        sch->sweep_y = 0;
    } else {
        sch->sweep_y = end_y;
    }
    return (rt_path_span_t){first_y, end_y};
}

rt_path_span_t
rt_path_schedule_advance(rt_path_schedule_t* sch, const rt_cornell_camera_t* cam, rt_path_target_t target,
                         int pixel_budget) {
    if (sch->step != 0) {
        return advance_seeding(sch, cam, target, pixel_budget);
    }
    if (target.accum == NULL) {
        return (rt_path_span_t){0, 0}; /* the fallback's one pass already drew everything */
    }
    return advance_sweep(sch, cam, target, pixel_budget);
}

void
rt_path_schedule_reset(rt_path_schedule_t* sch) {
    *sch = (rt_path_schedule_t){RT_REFINE_FIRST_STEP, 0, 0, 0};
}
