/*
 * rt_path - an accumulating path tracer over the same Cornell box
 * rt_cornell.h shades with one Whitted bounce: next-event estimation at
 * every hit, a cosine-weighted indirect bounce three deep, and a fixed-point
 * running-mean accumulator so a caller can fold in one more sample per pixel
 * per frame without a float sum that grows forever. Float throughout, same
 * reason as rt_cornell.c - see its own header.
 *
 * ESP-IDF-free and host-testable: no gfx.h, no screen size baked in - a
 * caller hands in the buffers and their width/height, same as
 * rt_cornell_camera_init() takes its viewport. The camera itself is
 * rt_cornell.h's own: same room, same lens, so nothing here repeats it.
 */
#pragma once

#include <stdint.h>

#include "gfx/gfx_color.h"
#include "render/r3d_ray.h"
#include "rt_cornell.h"

/* A running mean per channel, fixed point rather than a float sum: it
 * cannot overflow and does not lose precision as the sample count climbs,
 * at half the bytes a float accumulator would cost. */
typedef struct {
    uint16_t r, g, b;
} rt_path_accum_px_t;

/* The linear radiance a stored mean currently reads as - the resolve step's
 * input, and what a test compares a reference estimate against. */
r3d_vec3f_t rt_path_accum_radiance(rt_path_accum_px_t px);

/* Folds `sample` into `*px` as the n-th of an exact running mean:
 * mean += (sample - mean) / n. A constant `sample` stream leaves `*px`
 * unchanged from the second call on - integer division, so its own zero
 * delta never drifts. n == 1 on an unwritten (zeroed) `*px` is exact
 * seeding: the mean becomes `sample`. */
void rt_path_accum_add(rt_path_accum_px_t* px, r3d_vec3f_t sample, uint32_t n);

/* The seeding pass's own term: next-event direct light off the primary hit,
 * sampled at the light quad's own CENTRE rather than a random point - no
 * RNG at all, so a flat lit surface reads as a smooth falloff rather than
 * per-pixel noise. Deterministic in (x, y) alone. */
r3d_vec3f_t rt_path_direct_estimate(const rt_cornell_camera_t* cam, int x, int y);

/* One full path sample for pixel (x, y): next-event direct light at every
 * hit, then a cosine-weighted bounce, three hits deep. Deterministic in
 * (x, y, sample_index) alone - a hash seeds a small local generator, so
 * nothing here carries state between calls or needs a per-pixel RNG
 * buffer. The same (x, y, sample_index) always returns the same colour,
 * on host and on device alike. */
r3d_vec3f_t rt_path_sample(const rt_cornell_camera_t* cam, int x, int y, uint32_t sample_index);

/* The same integrator with every indirect bounce switched off - next-event
 * direct light at the primary hit alone, still a live random point on the
 * light quad each call (unlike rt_path_direct_estimate's fixed centre).
 * What a colour-bleed test compares a full rt_path_sample() against to
 * isolate the bounce's own contribution, rather than eyeballing a noisy
 * picture. */
r3d_vec3f_t rt_path_sample_direct_only(const rt_cornell_camera_t* cam, int x, int y, uint32_t sample_index);

/* Identity below a knee, Reinhard-shaped above it - a path estimate
 * routinely exceeds the 1.0 the plain Whitted picture never does, and
 * quantizing without compressing first would just clip the brightest
 * pixels flat. Ordinary, already-under-1.0 brightness passes through
 * unchanged rather than getting dimmed along with the highlights. */
r3d_vec3f_t rt_path_tonemap(r3d_vec3f_t linear);

/* Tone-mapped, ordered-dither resolve of a raw radiance value - the
 * fallback path when there is no accumulator to read from. */
gfx_color_t rt_path_resolve_radiance(r3d_vec3f_t radiance, int x, int y);

/* Tone-mapped, ordered-dither resolve of a stored running mean. */
gfx_color_t rt_path_resolve(rt_path_accum_px_t px, int x, int y);

/* Where an accumulating render is: `step` is rt_refine.h's own lattice
 * spacing while every pixel is still being seeded (0 once seeding is
 * done); `seed_y`/`sweep_y` are the next row each of those two phases has
 * not yet traced; `spp` is how many samples are merged into every pixel
 * that already has its full share - 0 until seeding completes, then one
 * higher per finished sweep of the whole screen. */
typedef struct {
    int step;
    int seed_y;
    int sweep_y;
    uint32_t spp;
} rt_path_schedule_t;

/* Back to the first seeding pass, sample count zero - what an invalidate()
 * or an orientation change asks for. Does not touch an accumulator; a
 * caller holding one clears it separately (there is no buffer here to
 * clear). */
void rt_path_schedule_reset(rt_path_schedule_t* sch);

/* `fb` and `accum` are `width * height` each, row-major, owned by the
 * caller. `accum` may be NULL: the allocation-failure fallback, direct
 * light only, rendered once through the same seeding lattice and never
 * revisited - see rt_path_schedule_advance()'s own comment. */
typedef struct {
    gfx_color_t* fb;
    rt_path_accum_px_t* accum;
    int width, height;
} rt_path_target_t;

/* The rows this call touched, half-open, for the caller's own dirty mark.
 * y1 == y0 when nothing was traced (a finished fallback render has nothing
 * left to do). */
typedef struct {
    int y0, y1;
} rt_path_span_t;

/* The seed pass's own per-row work: every new lattice pixel in [y0, y1) at
 * `step`, folded into `target.accum` when there is one. A step-aligned
 * split of [y0, y1), traced as two calls, draws the same picture as one -
 * what lets rt_path_schedule_advance() split it across two cores. */
void rt_path_seed_rows(const rt_cornell_camera_t* cam, rt_path_target_t target, int y0, int y1, int step);

/* The accumulate pass's own per-row work: folds path sample `n` into every
 * pixel of rows [y0, y1), full width. Splits the same way
 * rt_path_seed_rows() does. */
void rt_path_sweep_rows(const rt_cornell_camera_t* cam, rt_path_target_t target, int y0, int y1, uint32_t n);

/* Traces up to `pixel_budget` pixels' worth of work into `target.fb`,
 * resolving through `target.accum` when there is one: rt_refine.h's
 * coarse-to-fine lattice while seeding, then whole rows, top to bottom,
 * one more sample folded into every pixel per full sweep - rt_refine.h's
 * own header comment on row order applies here too. `target.accum == NULL`
 * seeds once, direct light only, and returns an empty span on every call
 * after: the fallback picture, never revisited. */
rt_path_span_t rt_path_schedule_advance(rt_path_schedule_t* sch, const rt_cornell_camera_t* cam,
                                        rt_path_target_t target, int pixel_budget);
