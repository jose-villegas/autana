/*
 * boot_anim: CPU-portable maths and timing for the startup animation: the
 * zeta function rising along its critical line.
 *
 * The curve, floor and timeline are integers. The camera and space tracks are
 * float transforms composed once a frame; each point then projects in integers
 * (render/r3d_project_x.h). One space unit is one metre, so perspective
 * foreshortens by distance.
 *
 * What is drawn: a floor gridded with the complex plane zeta's VALUE lives
 * in, t straight up for the height up the critical line, and
 * zeta(1/2 + it) for t from 0 to 126 plotted at height t, touching the t
 * axis at each nontrivial zero in that range (BOOT_ANIM_ZEROS of them).
 *
 * The curve table, floor and title use separate fixed-point scales:
 *
 *   Q12    a value of zeta. 4096 is 1.0, one unit of the floor grid.
 *   Q8     a height t. 256 is 1.0, and 126 * 256 still fits an int16.
 *   Q16.16 metres, where a point enters the view matrix; camera space is 1/512
 *          m (R3D_X_UNIT_ONE).
 *   Q15    sines and cosines from math/scalar/trig.h, used by the title's
 *          wobble/wave and the floor ripple, not by the camera.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "anim/anim_transform.h"
#include "anim/playback/anim_playhead.h"
#include "boot/boot_anim_curve.h"
#include "boot/boot_anim_timeline.h"
#include "core/handle.h"
#include "draw/draw_font.h"
#include "gfx/target/gfx_color.h"
#include "image/image.h"
#include "image/pixel/rgb565.h"
#include "image/pixel/rgb888.h"
#include "math/linear/transformf.h"
#include "math/linear/vec2i.h"
#include "math/linear/vec3f.h"
#include "math/scalar/ease.h"
#include "math/scalar/mathi.h"
#include "math/scalar/trig.h"
#include "render/r3d_project_x.h"
#include "render/render_view.h"

#define BOOT_ANIM_Q   12
#define BOOT_ANIM_ONE (1 << BOOT_ANIM_Q) /* 4096 == 1.0 */
#define BOOT_ANIM_TQ  8                  /* t's own fixed point */

/* The boot clip: its pack and its one entry, built from boot_anim_motion.anim.toml. */
#define BOOT_CLIP     "boot_anim_motion"
/* The boot photograph: its pack and its one picture entry. */
#define BOOT_PHOTO    "boot"

/* How the camera and the space move: the boot clip's nodes "camera" and
 * "space", pointing into its pack, or the rest pose when the clip cannot be
 * read. */
typedef struct {
    anim_clip_t clip;
    anim_node_tracks_t camera;
    anim_node_tracks_t space;
    bool from_pack;
    handle_t load;
} boot_anim_motion_t;

/* Fills `out` from the boot clip's pack, mounting it. On any failure it logs
 * why and fills the rest pose, so the animation still draws. */
void boot_anim_motion_load(boot_anim_motion_t* out);

/* Drops the pack boot_anim_motion_load() mounted, if it did, and leaves the
 * rest pose in `motion`, which no longer points into the pack. */
void boot_anim_motion_release(boot_anim_motion_t* motion);

/* Fills `out` with the photograph the animation crossfades to, the boot
 * picture's pack entry, mounting its pack. On any failure, including a picture
 * that is not one panel, it logs why and leaves no pixels, so the animation
 * draws on without it. */
void boot_anim_photo_load(image_t* out);

/* Drops the pack boot_anim_photo_load() mounted, if it did (`photo` has
 * pixels), and leaves no pixels in `photo`. */
void boot_anim_photo_release(image_t* photo);

typedef struct {
    transformf_t camera;
    transformf_t space;
} boot_anim_timeline_state_t;

static inline boot_anim_timeline_state_t
boot_anim_timeline_sample(const boot_anim_motion_t* motion, uint32_t now_ms) {
    anim_playhead_t playhead;
    anim_playhead_init(&playhead, ANIM_CLAMP);
    anim_playhead_advance(&playhead, &motion->clip, now_ms);
    const float seconds = anim_playhead_seconds(&playhead);
    boot_anim_timeline_state_t st;
    st.camera = anim_transform_sample(&motion->camera, seconds);
    st.space = anim_transform_sample(&motion->space, seconds);
    return st;
}

#define BOOT_ANIM_T_MAX        126 /* the top of the climb            */

/* Must match PHASE1_T_MAX in tools/gen/gen_zeta_curve.py. */
#define BOOT_ANIM_T_MAX_PHASE1 35

#define BOOT_ANIM_GRID_FADE    BOOT_ANIM_GRID_BANDS

/* The spiral: one unit of t climbs 132/512 of a meter. */
#define BOOT_ANIM_SPIRAL_Q9    132

typedef r3d_line_projector_t boot_anim_view_t;

boot_anim_view_t boot_anim_view(const boot_anim_motion_t* motion, int w, int h, uint32_t now_ms);

/* Q12 re/im and Q8 t convert exactly to Q16.16 meters; the view's Q9 matrix
 * lands them in camera space in 1/512 m. */
static inline vec3x_t
boot_anim_to_camera_space(int32_t re_q12, int32_t im_q12, int32_t t_q8, const boot_anim_view_t* view) {
    /* Inside R3D_X_INPUT_XZ_LIMIT on the floor's axes and
     * R3D_X_INPUT_Y_LIMIT on t the transform is three 32-bit products;
     * the long axes and a wild view take the 64-bit one. */
    const bool near = (uint32_t)(re_q12 + R3D_X_INPUT_XZ_LIMIT) < 2u * R3D_X_INPUT_XZ_LIMIT
                      && (uint32_t)(im_q12 + R3D_X_INPUT_XZ_LIMIT) < 2u * R3D_X_INPUT_XZ_LIMIT
                      && (uint32_t)(t_q8 + R3D_X_INPUT_Y_LIMIT) < 2u * R3D_X_INPUT_Y_LIMIT;
    if (near && view->units_ok) {
        return r3d_to_camera_space_units(view, re_q12, t_q8, im_q12);
    }
    const vec3x_t p = {re_q12 * 16, t_q8 * (BOOT_ANIM_SPIRAL_Q9 / 2), im_q12 * 16};

    return mat4x_apply(&view->matrix, p);
}

/* The camera-space image of the floor plane at one height: a point of it is
 * `origin` plus its re and im steps, so a ring of points at one height costs
 * two products a component instead of three, and no translation add; see
 * mathx_dot2c() for its rounding. All in 1/512 m. */
typedef struct {
    vec3x_t origin;
    vec3x_t re_step; /* the view matrix's re column, Q9 */
    vec3x_t im_step; /* its im column */
} boot_anim_plane_t;

static inline boot_anim_plane_t
boot_anim_plane(int32_t t_q8, const boot_anim_view_t* view) {
    const mat4x_t* m = &view->matrix;
    boot_anim_plane_t plane;
    plane.origin = boot_anim_to_camera_space(0, 0, t_q8, view);
    plane.re_step = (vec3x_t){m->m[0][0], m->m[1][0], m->m[2][0]};
    plane.im_step = (vec3x_t){m->m[0][2], m->m[1][2], m->m[2][2]};
    return plane;
}

static inline vec3x_t
boot_anim_plane_point(const boot_anim_plane_t* plane, int32_t re_q12, int32_t im_q12) {
    const int32_t re = re_q12 * 16;
    const int32_t im = im_q12 * 16;
    return (vec3x_t){
        mathx_dot2c(plane->re_step.x, re, plane->im_step.x, im, plane->origin.x),
        mathx_dot2c(plane->re_step.y, re, plane->im_step.y, im, plane->origin.y),
        mathx_dot2c(plane->re_step.z, re, plane->im_step.z, im, plane->origin.z),
    };
}

/* False when the point is at or behind the near plane. */
static inline bool
boot_anim_project_point(int32_t re_q12, int32_t im_q12, int32_t t_q8, const boot_anim_view_t* view, int* screen_x,
                        int* screen_y) {
    const vec3x_t p = boot_anim_to_camera_space(re_q12, im_q12, t_q8, view);
    return r3d_project_point_cs_x(p, view, screen_x, screen_y);
}

static inline bool
boot_anim_project_segment(int32_t re0, int32_t im0, int32_t t0, int32_t re1, int32_t im1, int32_t t1,
                          const boot_anim_view_t* view, int* ax, int* ay, int* bx, int* by) {
    return r3d_project_segment_cs_x(boot_anim_to_camera_space(re0, im0, t0, view),
                                    boot_anim_to_camera_space(re1, im1, t1, view), view, ax, ay, bx, by);
}

static inline int32_t
boot_anim_zeta_to_t_q8(int32_t zeta_q12) {
    return (int32_t)(((int64_t)zeta_q12 * 32) / BOOT_ANIM_SPIRAL_Q9);
}

typedef struct {
    int32_t radius;
    int32_t amplitude;
    int32_t calm_start;
    uint16_t inv_width;
    uint16_t ripple_phase;
    uint16_t swell;
    uint16_t far_swell; /* how much of it the far lines keep, eased out before the crossfade */
    bool calm;
    uint16_t phase[BOOT_ANIM_SWELL_COUNT];
    int32_t far_reach; /* Q12: how far out the floor reaches */
} boot_anim_floor_state_t;

/* Time samples interpolate across phase wrap; spatial tables are nearest samples. */
static inline boot_anim_floor_state_t
boot_anim_floor_state(uint32_t now_ms) {
    const uint32_t ms = now_ms < BOOT_ANIM_MS ? now_ms : BOOT_ANIM_MS;
    const unsigned index = ms / BOOT_ANIM_FLOOR_TIME_STEP_MS;
    const int fraction = (int)(ms % BOOT_ANIM_FLOOR_TIME_STEP_MS);
    const boot_anim_floor_time_t* a = &boot_anim_floor_times[index];
    const boot_anim_floor_time_t* b = a + 1;
    boot_anim_floor_state_t state = {
        .radius = mathi_lerp(a->radius, b->radius, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .amplitude = mathi_lerp(a->amplitude, b->amplitude, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .calm_start = mathi_lerp(a->calm_start, b->calm_start, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .inv_width = mathi_lerp(a->inv_width, b->inv_width, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .ripple_phase = (uint16_t)(a->ripple_phase
                                   + mathi_lerp(0, (int16_t)(b->ripple_phase - a->ripple_phase), fraction,
                                                BOOT_ANIM_FLOOR_TIME_STEP_MS)),
        .swell = mathi_lerp(a->swell, b->swell, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .far_swell = mathi_lerp(a->far_swell, b->far_swell, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .far_reach = mathi_lerp(a->far_reach, b->far_reach, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS),
        .calm = ms > BOOT_ANIM_RIPPLE_IMPACT_MS + BOOT_ANIM_RIPPLE_CALM_AFTER_MS,
    };
    if (now_ms <= BOOT_ANIM_RIPPLE_IMPACT_MS) {
        state.amplitude = 0;
    }
    if (now_ms <= BOOT_ANIM_TITLE_START_MS) {
        state.swell = 0;
    }
    for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
        const int delta = (int16_t)(b->phase[k] - a->phase[k]);
        state.phase[k] = (uint16_t)(a->phase[k] + mathi_lerp(0, delta, fraction, BOOT_ANIM_FLOOR_TIME_STEP_MS));
    }
    return state;
}

static inline const boot_anim_floor_node_t*
boot_anim_floor_node(int i, int j) {
    const int side = (BOOT_ANIM_MESH_CELLS / 2) + 1;
    return &boot_anim_floor_nodes[(mathi_abs(j) * side) + mathi_abs(i)];
}

/* Heights are Q12 metres; only the impact contributes to crest colour. */
static inline int32_t
boot_anim_ripple_at(const boot_anim_floor_node_t* node, const boot_anim_floor_state_t* state) {
    const int x = ((uint32_t)mathi_abs(node->radius - state->radius) * state->inv_width) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
    if (x > BOOT_ANIM_RIPPLE_GAUSSIAN_EXTENT) {
        return 0;
    }
    int32_t amplitude = (state->amplitude * boot_anim_ripple_gaussian[x]) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
    if (state->calm) {
        const int u = mathi_clamp((mathi_clamp(node->radius - state->calm_start, 0, BOOT_ANIM_RIPPLE_WAVELENGTH_SHAPE)
                                   * BOOT_ANIM_RIPPLE_INV_WAVELENGTH)
                                      >> BOOT_ANIM_FLOOR_GAIN_SHIFT,
                                  0, BOOT_ANIM_RIPPLE_LOOKUP_SCALE);
        amplitude = (amplitude * boot_anim_ripple_smoothstep[u]) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
    }
    return (-amplitude * trig_cos((uint16_t)(node->phase + state->ripple_phase))) >> TRIG_SIN_SHIFT;
}

static inline int32_t
boot_anim_floor_ripple(int i, int j, const boot_anim_floor_state_t* state) {
    return boot_anim_ripple_at(boot_anim_floor_node(i, j), state);
}

/* The swell at a place whose spatial phases are `place`. */
static inline int32_t
boot_anim_swell_at(const uint16_t place[BOOT_ANIM_SWELL_COUNT], const boot_anim_floor_state_t* state) {
    if (state->swell == 0) {
        return 0;
    }
    int32_t swell = 0;
    for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
        const uint16_t phase = (uint16_t)(place[k] + state->phase[k]);
        swell += (boot_anim_swell_weights[k] * trig_sin(phase)) >> TRIG_SIN_SHIFT;
    }
    return (swell * state->swell) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
}

/* The most the swell can lift or lower any place at this moment. */
static inline int32_t
boot_anim_swell_bound(const boot_anim_floor_state_t* state) {
    int32_t bound = 0;
    for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
        bound += mathi_abs(boot_anim_swell_weights[k]);
    }
    return (bound * state->swell) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
}

static inline int32_t
boot_anim_floor_swell(int i, int j, const boot_anim_floor_state_t* state) {
    uint16_t place[BOOT_ANIM_SWELL_COUNT];
    for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
        place[k] = (uint16_t)(boot_anim_swell_re[i + (BOOT_ANIM_MESH_CELLS / 2)][k]
                              + boot_anim_swell_im[j + (BOOT_ANIM_MESH_CELLS / 2)][k]);
    }
    return boot_anim_swell_at(place, state);
}

/* The swell past the mesh, `gain` of it kept (BOOT_ANIM_FLOOR_GAIN_SHIFT):
 * it eases out toward BOOT_ANIM_FAR_SWELL_FADE_Q12 so the horizon stays flat. */
static inline int32_t
boot_anim_far_swell_at(const uint16_t place[BOOT_ANIM_SWELL_COUNT], uint16_t gain,
                       const boot_anim_floor_state_t* state) {
    if (gain == 0) {
        return 0;
    }
    return (boot_anim_swell_at(place, state) * gain) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
}

/* The height of axis sample `n` out along (dir_re, dir_im), one of the four
 * unit directions: past the mesh only the lines on the axes go on. */
static inline int32_t
boot_anim_axis_far_height(int n, int dir_re, int dir_im, const boot_anim_floor_state_t* state) {
    const boot_anim_axis_far_t* sample = &boot_anim_axis_far[n];
    uint16_t place[BOOT_ANIM_SWELL_COUNT];
    for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
        place[k] = (uint16_t)((dir_re * sample->swell_re[k]) + (dir_im * sample->swell_im[k]));
    }
    return boot_anim_ripple_at(&sample->node, state) + boot_anim_far_swell_at(place, sample->swell_gain, state);
}

/* The swell at sample `n` of far line `line`: the line runs along im
 * (along_im) or re, at `side` (+1 or -1) times its distance from the axis,
 * and the sample is on its `half` (+1 or -1) of it. The far lines keep
 * `far_swell` of it, none from the crossfade on. */
static inline int32_t
boot_anim_far_line_swell(const boot_anim_far_line_t* line, int n, bool along_im, int side, int half,
                         const boot_anim_floor_state_t* state) {
    const boot_anim_far_sample_t* sample = &boot_anim_far_samples[line->first + n];
    const uint16_t* across = along_im ? line->swell_re : line->swell_im;
    const uint16_t* along = along_im ? sample->swell_im : sample->swell_re;
    uint16_t place[BOOT_ANIM_SWELL_COUNT];
    for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
        place[k] = (uint16_t)((side * across[k]) + (half * along[k]));
    }
    const uint16_t gain = (uint16_t)(((uint32_t)sample->swell_gain * state->far_swell) >> BOOT_ANIM_FLOOR_GAIN_SHIFT);
    return boot_anim_far_swell_at(place, gain, state);
}

static inline int32_t
boot_anim_floor_height(int i, int j, const boot_anim_floor_state_t* state) {
    return boot_anim_floor_ripple(i, j, state) + boot_anim_floor_swell(i, j, state);
}

/* How far a crest of `ripple` has turned from the floor's grey, 0..255:
 * squared, so only the tops take colour. */
static inline uint8_t
boot_anim_crest_amount(int32_t ripple) {
    const int32_t crest = (mathi_clamp(ripple, 0, BOOT_ANIM_CREST_HEIGHT_Q12) * 255) / BOOT_ANIM_CREST_HEIGHT_Q12;
    return (uint8_t)((crest * crest) / 255);
}

static inline gfx_color_t
boot_anim_floor_color(int32_t ripple, uint32_t crest_rgb) {
    return rgb565be_mix(RGB565BE(BOOT_ANIM_FLOOR_GREY), RGB565BE(crest_rgb), boot_anim_crest_amount(ripple));
}

/* The curve sample nearest mesh node (i, j) in the floor plane among the
 * first `drawn` + 1, from the baked runs of every BOOT_ANIM_CREST_STRIDE-th
 * node: inside a run the pen's own sample is the nearest, between runs the
 * last one holds. */
static inline int
boot_anim_crest_sample(int i, int j, int drawn) {
    const int half = BOOT_ANIM_MESH_CELLS / 2;
    const int ci = (i + half + (BOOT_ANIM_CREST_STRIDE / 2)) / BOOT_ANIM_CREST_STRIDE;
    const int cj = (j + half + (BOOT_ANIM_CREST_STRIDE / 2)) / BOOT_ANIM_CREST_STRIDE;
    const int node = (cj * BOOT_ANIM_CREST_SIDE) + ci;
    int sample = 0;
    for (int r = boot_anim_crest_run_start[node]; r < boot_anim_crest_run_start[node + 1]; r++) {
        if (boot_anim_crest_runs[r][0] > drawn) {
            break;
        }
        sample = mathi_min(drawn, boot_anim_crest_runs[r][1]);
    }
    return sample;
}

#define BOOT_ANIM_LOD_STRIDE2_PX        192
#define BOOT_ANIM_LOD_STRIDE4_PX        96

#define BOOT_ANIM_DISSOLVE_COARSE_LEVEL 8

typedef struct {
    int32_t re, im; /* Q12 */
    int32_t t;      /* Q8  */
} boot_anim_pt_t;

/* A quadratic B-spline through c0, c1, c2 at `t_q12` for BOOT_ANIM_ONE.
 * Avoids Catmull-Rom's overshoot, staying convex. Quadratic for simpler
 * normalization (shift vs divide). */
static inline boot_anim_pt_t
boot_anim_spline(boot_anim_pt_t c0, boot_anim_pt_t c1, boot_anim_pt_t c2, int32_t t_q12) {
    /* 32-bit throughout, deliberately not math/scalar/fixed.h's widening helpers:
     * the operands are sized so the product cannot overflow an int32, on a
     * path that runs several thousand times a frame. */
    const int32_t u = BOOT_ANIM_ONE - t_q12;
    const int32_t w0 = (u * u) >> BOOT_ANIM_Q;
    const int32_t w2 = (t_q12 * t_q12) >> BOOT_ANIM_Q;

    /* Weights sum to 2.0 by construction, so the middle one is never
     * computed from its own polynomial and cannot drift from the other
     * two. */
    const int32_t w1 = (2 * BOOT_ANIM_ONE) - w0 - w2;

    boot_anim_pt_t p;
    p.re = (w0 * c0.re + w1 * c1.re + w2 * c2.re) >> (BOOT_ANIM_Q + 1);
    p.im = (w0 * c0.im + w1 * c1.im + w2 * c2.im) >> (BOOT_ANIM_Q + 1);
    p.t = (w0 * c0.t + w1 * c1.t + w2 * c2.t) >> (BOOT_ANIM_Q + 1);
    return p;
}

/* Same quadratic B-spline as boot_anim_spline() above, on points already
 * in CAMERA space, avoiding a transform per sub-point. Exact, not
 * approximate: boot_anim_to_camera_space() is an affine map and this
 * spline's weights sum to a constant, so it commutes with this
 * combination exactly. */
static inline vec3x_t
boot_anim_spline_cs(vec3x_t c0, vec3x_t c1, vec3x_t c2, int32_t t_q12) {
    const int32_t u = BOOT_ANIM_ONE - t_q12;
    const int32_t w0 = (u * u) >> BOOT_ANIM_Q;
    const int32_t w2 = (t_q12 * t_q12) >> BOOT_ANIM_Q;
    const int32_t w1 = (2 * BOOT_ANIM_ONE) - w0 - w2;

    /* int64: a camera-space coordinate has no known-small-range promise a raw
     * curve-table value does. */
    return (vec3x_t){
        (int32_t)((((int64_t)w0 * c0.x) + ((int64_t)w1 * c1.x) + ((int64_t)w2 * c2.x)) >> (BOOT_ANIM_Q + 1)),
        (int32_t)((((int64_t)w0 * c0.y) + ((int64_t)w1 * c1.y) + ((int64_t)w2 * c2.y)) >> (BOOT_ANIM_Q + 1)),
        (int32_t)((((int64_t)w0 * c0.z) + ((int64_t)w1 * c1.z) + ((int64_t)w2 * c2.z)) >> (BOOT_ANIM_Q + 1)),
    };
}

/* Clamping pins spline to ends. Repeated first control point starts first
 * span at first sample. */
static inline boot_anim_pt_t
boot_anim_sample(int i) {
    if (i < 0) {
        i = 0;
    } else if (i >= BOOT_ANIM_CURVE_POINTS) {
        i = BOOT_ANIM_CURVE_POINTS - 1;
    }
    boot_anim_pt_t p;
    p.re = boot_anim_curve[i].re;
    p.im = boot_anim_curve[i].im;
    p.t = boot_anim_curve[i].t;
    return p;
}

/* The curve table's points, which the generator keeps well inside the narrow
 * transform's inputs (test_the_curve_table_stays_inside_the_narrow_range()):
 * no per-call range test, only the view's own flag. */
static inline vec3x_t
boot_anim_key_to_camera_space(const boot_anim_pt_t* key, const boot_anim_view_t* view) {
    if (view->units_ok) {
        return r3d_to_camera_space_units(view, key->re, key->t, key->im);
    }
    return boot_anim_to_camera_space(key->re, key->im, key->t, view);
}

static inline int
boot_anim_lod_stride_for_extent(int32_t manhattan_px) {
    if (manhattan_px < BOOT_ANIM_LOD_STRIDE4_PX) {
        return 4;
    }
    if (manhattan_px < BOOT_ANIM_LOD_STRIDE2_PX) {
        return 2;
    }
    return 1;
}

static inline int
boot_anim_curve_stride(const boot_anim_view_t* view) {
    static const int probe_idx[3] = {
        0,
        BOOT_ANIM_CURVE_POINTS / 2,
        BOOT_ANIM_CURVE_POINTS - 1,
    };
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;

    for (int k = 0; k < 3; k++) {
        const boot_anim_pt_t p = boot_anim_sample(probe_idx[k]);
        int sx, sy;
        if (!boot_anim_project_point(p.re, p.im, p.t, view, &sx, &sy)) {
            return 1;
        }
        if (k == 0 || sx < min_x) {
            min_x = sx;
        }
        if (k == 0 || sx > max_x) {
            max_x = sx;
        }
        if (k == 0 || sy < min_y) {
            min_y = sy;
        }
        if (k == 0 || sy > max_y) {
            max_y = sy;
        }
    }

    return boot_anim_lod_stride_for_extent((max_x - min_x) + (max_y - min_y));
}

/* Every phase takes milliseconds since the animation started, so timing is
 * frame-rate independent. */

/* A FRACTION, not a pixel count: arms have different lengths. */
static inline uint8_t
boot_anim_axis_reach(uint32_t now_ms) {
    return ease_out_u8(ease_ramp_u8(now_ms, 0, BOOT_ANIM_AXES_MS));
}

/* Band by band, outward: how far band `band` has faded in. */
static inline uint8_t
boot_anim_grid_arrival(uint32_t now_ms, int band) {
    const uint32_t start = BOOT_ANIM_GRID_START_MS + ((uint32_t)band * BOOT_ANIM_GRID_BAND_MS);
    return ease_ramp_u8(now_ms, start, BOOT_ANIM_GRID_FADE_MS);
}

static inline uint8_t
boot_anim_grid_alpha(uint32_t now_ms, int band) {
    if (band >= BOOT_ANIM_GRID_FADE) {
        return 0;
    }
    const uint32_t arrived = boot_anim_grid_arrival(now_ms, band);
    const uint32_t left = (uint32_t)(BOOT_ANIM_GRID_FADE - band);
    const uint32_t ramp = BOOT_ANIM_GRID_FADE - BOOT_ANIM_GRID_EDGE_FADE_BAND;
    return (uint8_t)(arrived * mathi_min(left, ramp) / ramp);
}

/* The title is laid out in the viewer's frame because the board is held one
 * quarter turn from the panel's native orientation. */

#define BOOT_ANIM_TITLE        "Autana"
#define BOOT_ANIM_TITLE_LEN    6

#define BOOT_ANIM_TITLE_GAP    3 /* extra px of tracking between glyphs */

#define BOOT_ANIM_TITLE_VIEW_W 448
#define BOOT_ANIM_TITLE_VIEW_H 368

static inline int
boot_anim_title_wobble(int32_t d_q12) {
    if (d_q12 <= 0) {
        return 0;
    }
    const int32_t d2_q12 = (d_q12 * d_q12) >> BOOT_ANIM_Q;

    const uint16_t phase = (uint16_t)((d2_q12 * BOOT_ANIM_TITLE_TURNS_PHASE) >> BOOT_ANIM_Q);

    const int32_t amp = (BOOT_ANIM_TITLE_AMPLITUDE_PX * d_q12) >> BOOT_ANIM_Q;
    return (int)((amp * trig_sin(phase)) >> TRIG_SIN_SHIFT);
}

static inline uint8_t
boot_anim_title_wave_reach(uint32_t now_ms) {
    return (uint8_t)(255u - ease_ramp_u8(now_ms, BOOT_ANIM_TITLE_WAVE_OUT_MS, BOOT_ANIM_TITLE_WAVE_FADE_MS));
}

static inline int
boot_anim_title_wave(int i, uint32_t now_ms) {
    const uint32_t t = (now_ms + (uint32_t)i * BOOT_ANIM_TITLE_WAVE_STAGGER_MS) % BOOT_ANIM_TITLE_WAVE_PERIOD_MS;
    const uint16_t phase = (uint16_t)((t * TRIG_TURN) / BOOT_ANIM_TITLE_WAVE_PERIOD_MS);
    const int32_t amp = (BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX * boot_anim_title_wave_reach(now_ms)) / 255;
    return (int)((amp * trig_sin(phase)) >> TRIG_SIN_SHIFT);
}

/* Row starts at a FIXED BOOT_ANIM_TITLE_VIEW_X/Y, not read live: the
 * target a letter flies toward has to stay put for the whole flight.
 * final_x sums each letter's real advance via draw_font_text_width(), not
 * `i * cell_w`: a fixed per-cell reckoning is wrong for a proportional
 * font, where narrow/wide glyphs do not share one width; see
 * test_final_x_matches_the_advance_sum() in suite_boot_anim.c. */
static inline vec2i_t
boot_anim_title_letter(const draw_font_t* font, int i, uint32_t now_ms) {
    const uint32_t start = BOOT_ANIM_TITLE_START_MS + ((uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS);
    const uint8_t u8 = ease_out_u8(ease_ramp_u8(now_ms, start, BOOT_ANIM_TITLE_FLIGHT_MS));

    const int prefix_w = draw_font_text_width(font, BOOT_ANIM_TITLE, i, BOOT_ANIM_TITLE_SCALE);
    const int final_x = BOOT_ANIM_TITLE_VIEW_X + prefix_w + (i * BOOT_ANIM_TITLE_GAP);
    const int start_x = final_x - BOOT_ANIM_TITLE_ENTRY_PX;

    vec2i_t p;
    p.x = mathi_lerp_u8(start_x, final_x, u8);

    const int32_t d_q12 = BOOT_ANIM_ONE - mathi_lerp_u8(0, BOOT_ANIM_ONE, u8);
    p.y = BOOT_ANIM_TITLE_VIEW_Y + boot_anim_title_wobble(d_q12) + boot_anim_title_wave(i, now_ms);
    return p;
}

/* Swap and negate offset for shadow. Pure, host-testable function. */
static inline void
boot_anim_title_shadow_offset(int dx, int dy, int* panel_dx, int* panel_dy) {
    *panel_dx = -dy;
    *panel_dy = dx;
}

/* This is a fraction of arc length, so a constant walk rate gives a constant
 * pen speed. The second phase finishes at its authored time and may overlap
 * the dissolve. */
#define BOOT_ANIM_CURVE_PHASE1_FRACTION                                                                                \
    ((int32_t)(((int64_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1) * BOOT_ANIM_ONE) / (BOOT_ANIM_CURVE_POINTS - 1)))

static inline int32_t
boot_anim_pen(uint32_t now_ms) {
    const uint32_t phase1_end_ms = BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS;

    if (now_ms <= phase1_end_ms) {
        const uint8_t linear = ease_ramp_u8(now_ms, BOOT_ANIM_PEN_START_MS, BOOT_ANIM_PEN_MS);
        return mathi_lerp_u8(0, BOOT_ANIM_CURVE_PHASE1_FRACTION, linear);
    }

    const uint8_t linear2 = ease_ramp_u8(now_ms, phase1_end_ms, BOOT_ANIM_PEN_FINISH_MS - phase1_end_ms);
    return mathi_lerp_u8(BOOT_ANIM_CURVE_PHASE1_FRACTION, BOOT_ANIM_ONE, linear2);
}

static inline int32_t
boot_anim_colour_progress(uint32_t now_ms) {
    const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);
    return (int32_t)(((int64_t)boot_anim_pen(now_ms) * span) / phase1_span);
}

static inline uint8_t
boot_anim_ink(uint32_t now_ms) {
    return (uint8_t)(255u - ease_ramp_u8(now_ms, BOOT_ANIM_FADE_START_MS, BOOT_ANIM_MS - BOOT_ANIM_FADE_START_MS));
}

/* Crossfade to the photograph, 0..255, from BOOT_ANIM_IMAGE_START_MS for
 * BOOT_ANIM_IMAGE_FADE_MS. Plain ease_ramp_u8(), not eased: ease_out(r) + ease_out(255-r)
 * is NOT 255 at every r, so easing either half of a cross-dissolve makes
 * the midpoint read brighter than either end; linear is what keeps the
 * two halves summing to one whole picture throughout. The scene shows
 * through what the reveal leaves, 255 - reveal. */
static inline uint8_t
boot_anim_photo_reveal(uint32_t now_ms) {
    return ease_ramp_u8(now_ms, BOOT_ANIM_IMAGE_START_MS, BOOT_ANIM_IMAGE_FADE_MS);
}

/* The frame, counted from 0, that mounts the photograph's pack. Mounting
 * checks the whole pack, about 20 ms on the board: on the first frame it
 * would delay the first picture, and in the crossfade it would stall a frame
 * already among the dearest. The early frames are the cheapest (about 15 ms),
 * so it takes the second. */
#define BOOT_ANIM_PHOTO_MOUNT_FRAME 1U

/* Whether frame `frame` (counted from 0) mounts the photograph's pack, if no
 * frame before it tried. */
static inline bool
boot_anim_photo_due(uint32_t frame) {
    return frame >= BOOT_ANIM_PHOTO_MOUNT_FRAME;
}

#define BOOT_ANIM_HUE_START   875  /* azure, at the foot of the climb */
#define BOOT_ANIM_HUE_SWEEP   1200 /* most of a turn by the top       */

/* Five pens show gamut with colour wash */
#define BOOT_ANIM_TRAILS      5

/* How far apart the pens run, as a Q12 fraction of the whole curve. */
#define BOOT_ANIM_TRAIL_GAP   768

#define BOOT_ANIM_TRAIL_SHIFT 10
#define BOOT_ANIM_TRAIL_Q12   (1 << BOOT_ANIM_TRAIL_SHIFT)

/* Heavy strokes drawn wider for visibility. */
#define BOOT_ANIM_FAT_TRAIL   150
#define BOOT_ANIM_FAT_WIDTH   3

static inline int32_t
boot_anim_trail_pos(int32_t pen_q12, int k) {
    return pen_q12 - ((int32_t)k * BOOT_ANIM_TRAIL_GAP);
}

/* The wheel position pen `k` carries, spread evenly round it. */
static inline int
boot_anim_trail_hue(int k) {
    return k * (RGB888_HUE_TURN / BOOT_ANIM_TRAILS);
}

typedef struct {
    int hue;       /* wheel position; rgb888_hue() wraps it */
    uint8_t bloom; /* mix that far toward white                    */
    uint8_t glow;  /* mix that far up from the background          */
    uint8_t width; /* pixels across                                */
} boot_anim_stroke_t;

/* BASE depends on position. Strongest pen wins outright rather than
 * summing: summing saturates wherever trails overlap, which is constantly,
 * smearing bands into one bright stretch. Hue mixed BY STRENGTH so a piece
 * halfway between two pens comes out halfway between their colours.
 * Falloff is linear, not squared: squared piles glow right behind the
 * head and is over almost at once, and a slow fade is the point of a
 * trail. */
static inline boot_anim_stroke_t
boot_anim_stroke(int32_t along_q12, int32_t pen_q12) {
    boot_anim_stroke_t s;

    const int32_t base_along = along_q12 > BOOT_ANIM_ONE ? BOOT_ANIM_ONE : along_q12;
    const int32_t base_glow = 132 + ((base_along * 60) >> BOOT_ANIM_Q);
    const int32_t base_bloom = (base_along * 24) >> BOOT_ANIM_Q;
    const int base_hue = BOOT_ANIM_HUE_START + (int)((along_q12 * BOOT_ANIM_HUE_SWEEP) >> BOOT_ANIM_Q);

    int32_t best = 0;
    int best_hue = 0;

    for (int k = 0; k < BOOT_ANIM_TRAILS; k++) {
        const int32_t behind = boot_anim_trail_pos(pen_q12, k) - along_q12;
        if (behind < 0 || behind >= BOOT_ANIM_TRAIL_Q12) {
            continue; /* this piece is ahead of that pen, or long past it */
        }
        const int32_t trail = 255 - ((behind * 255) >> BOOT_ANIM_TRAIL_SHIFT);
        if (trail > best) {
            best = trail;
            best_hue = boot_anim_trail_hue(k);
        }
    }

    /* Shift is cheaper, +1 ensures full brightness. */
    s.hue = base_hue + ((best_hue * (best + 1)) >> 8);
    s.glow = (uint8_t)(base_glow + (((255 - base_glow) * (best + 1)) >> 8));
    s.bloom = (uint8_t)(base_bloom + (((90 - base_bloom) * (best + 1)) >> 8));
    s.width = best >= BOOT_ANIM_FAT_TRAIL ? BOOT_ANIM_FAT_WIDTH : 1;

    return s;
}

/* The colour curve sample `sample` is drawn in at colour progress
 * `colour_pen`, without bloom or glow: what a crest under it turns toward. */
static inline uint32_t
boot_anim_crest_rgb(int sample, int32_t colour_pen) {
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);
    const int32_t along = ((sample * BOOT_ANIM_ONE) + (phase1_span / 2)) / phase1_span;
    return rgb888_hue(boot_anim_stroke(along, colour_pen).hue);
}

/* Axes reach their full extent over the fade interval, so the scene has one
 * ending. */
static inline uint8_t
boot_anim_finale_reach(uint32_t now_ms) {
    return ease_out_u8(ease_ramp_u8(now_ms, BOOT_ANIM_FADE_START_MS, BOOT_ANIM_MS - BOOT_ANIM_FADE_START_MS));
}

/* Axes run past the panel and rely on clipping. */
#define BOOT_ANIM_AXIS_FAR_UNITS 500

/* Host render tests call this directly and read the firmware framebuffer.
 * `photo` with no pixels draws the scene through to the end in its place. */
void boot_anim_draw_frame(const boot_anim_motion_t* motion, const image_t* photo, uint32_t now_ms);

/* What the picture dissolves INTO. Unset, the last frames fade to black and
 * whatever follows cuts in. Set, each of them starts from `paint`'s picture
 * of the screen that follows, and the photograph and title dither away over
 * it. The caller paints it because boot/ knows nothing above itself. */
typedef void (*boot_anim_backdrop_fn)(void);
void boot_anim_set_ending_backdrop(boot_anim_backdrop_fn paint);

#ifdef ESP_PLATFORM
/* Runs pre-boot, post-gfx_init(). Yields frames for watchdog. Device-only:
 * adds real wall clock and gfx_present() to real panel. */
void boot_anim_run(void);
#endif
