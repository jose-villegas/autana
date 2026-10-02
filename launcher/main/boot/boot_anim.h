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
 *   Q8     a height t. 256 is 1.0, and 35 * 256 still fits an int16.
 *   Q16.16 metres, where a point enters the view matrix; camera space is 1/512
 *          m (R3D_X_UNIT_ONE).
 *   Q15    sines and cosines from this file's own trig table, used by the
 *          title's wobble/wave, not by the camera.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "anim/anim_track.h"
#include "anim/anim_transform.h"
#include "boot/boot_anim_curve.h"
#include "boot/boot_anim_timeline.h"
#include "boot/boot_anim_tracks_generated.h"
#include "gfx/gfx_font.h"
#include "render/r3d_line_camera.h"
#include "render/r3d_project_x.h"
#include "util/intmath.h"
#include "util/math/transformf.h"
#include "util/math/vec2i.h"
#include "util/math/vec3f.h"
#include "util/trig.h"
#include "util/tween.h"

#define BOOT_ANIM_Q   12
#define BOOT_ANIM_ONE (1 << BOOT_ANIM_Q) /* 4096 == 1.0 */
#define BOOT_ANIM_TQ  8                  /* t's own fixed point */

typedef struct {
    transformf_t camera;
    transformf_t space;
} boot_anim_timeline_state_t;

static inline boot_anim_timeline_state_t
boot_anim_timeline_sample(uint32_t now_ms) {
    const float seconds = anim_clip_seconds(&boot_anim_clip, now_ms, ANIM_CLAMP);
    boot_anim_timeline_state_t st;
    st.camera = anim_transform_sample(&boot_anim_camera_translation, &boot_anim_camera_rotation,
                                      &boot_anim_camera_scale, seconds);
    st.space =
        anim_transform_sample(&boot_anim_space_translation, &boot_anim_space_rotation, &boot_anim_space_scale, seconds);
    return st;
}

#define BOOT_ANIM_T_MAX        126 /* the top of the climb            */

/* Must match PHASE1_T_MAX in tools/gen/gen_zeta_curve.py. */
#define BOOT_ANIM_T_MAX_PHASE1 35

/* RINGS is how far the fade reaches, not the floor size: quarter-unit
 * spacing (not whole) so it reads as a dense ripple rather than
 * individually countable bands. Both generated: RINGS in
 * boot_anim_timeline.h, BOOT_ANIM_GRID_STEP_Q12 with it. FADE is its own name for what
 * boot_anim_grid_alpha() means, but must equal RINGS exactly: short of
 * it is a hard edge, past it divides by a count nothing reaches. */
#define BOOT_ANIM_GRID_FADE    BOOT_ANIM_GRID_RINGS

/* The spiral: one unit of t climbs 132/512 of a meter. */
#define BOOT_ANIM_SPIRAL_Q9    132

typedef r3d_line_view_x_t boot_anim_view_t;

static inline boot_anim_view_t
boot_anim_view(int w, int h, uint32_t now_ms) {
    boot_anim_timeline_state_t st = boot_anim_timeline_sample(now_ms);

    const r3d_line_camera_t camera = {.pose = st.camera, .focal = BOOT_ANIM_CAMERA_FOCAL, .near_z = R3D_LINE_NEAR_Z};
    /* w (the panel's native WIDTH) is narrower than h (its native HEIGHT),
     * so r3d_line_camera_view()'s shorter-axis fit is exactly half w; boot's
     * pixels must not move if that inequality ever changes. */
    const viewport_t viewport = {.width = w, .height = h, .quarter = 0};
    const r3d_line_view_t view = r3d_line_camera_view(camera, &st.space, viewport);
    r3d_line_view_x_t out = r3d_line_view_to_x(&view);
    /* Meters per raw unit of (re, t, im): Q12, Q8 climbing 132/512 m a unit, Q12. */
    const float meters_per_unit[3] = {1.0F / (float)BOOT_ANIM_ONE,
                                      (float)BOOT_ANIM_SPIRAL_Q9 / (256.0F * (float)R3D_X_UNIT_ONE),
                                      1.0F / (float)BOOT_ANIM_ONE};
    r3d_line_view_x_set_inputs(&out, &view, meters_per_unit);
    return out;
}

/* Q12 re/im and Q8 t convert exactly to Q16.16 metres; the view's Q9 matrix
 * lands them in camera space in 1/512 m. */
static inline vec3x_t
boot_anim_to_camera_space(int32_t re_q12, int32_t im_q12, int32_t t_q8, const boot_anim_view_t* view) {
    /* Inside +-2^17 (32 m) on the floor's axes and +-2^15 on t the transform is
     * three 32-bit products; the long axes and a wild view take the 64-bit one. */
    const bool near = (uint32_t)(re_q12 + (1 << 17)) < (1u << 18) && (uint32_t)(im_q12 + (1 << 17)) < (1u << 18)
                      && (uint32_t)(t_q8 + (1 << 15)) < (1u << 16);
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

static inline void
boot_anim_project(int32_t re_q12, int32_t im_q12, int32_t t_q8, const boot_anim_view_t* view, int* screen_x,
                  int* screen_y) {
    const vec3x_t p = boot_anim_to_camera_space(re_q12, im_q12, t_q8, view);
    r3d_camera_to_screen_x(p, view, screen_x, screen_y);
}

/* False when the point is behind the near plane. */
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

#define BOOT_ANIM_WAVE_ENVELOPE_RAMP_MS 500

/* BOOT_ANIM_WAVE_IN_MS, BOOT_ANIM_WAVE_ENVELOPE_RAMP_MS,
 * BOOT_ANIM_WAVE_OUT_MS define shape. */
static inline uint8_t
boot_anim_wave_envelope(uint32_t now_ms) {
    const uint8_t in = tween_ramp(now_ms, BOOT_ANIM_WAVE_IN_MS, BOOT_ANIM_WAVE_ENVELOPE_RAMP_MS);
    const uint8_t out = (uint8_t)(255u - tween_ramp(now_ms, BOOT_ANIM_WAVE_OUT_MS, BOOT_ANIM_WAVE_ENVELOPE_RAMP_MS));

    return (in < out) ? in : out;
}

static inline int32_t
boot_anim_zeta_to_t_q8(int32_t zeta_q12) {
    return (int32_t)(((int64_t)zeta_q12 * 32) / BOOT_ANIM_SPIRAL_Q9);
}

/* A radial sine is periodic in r, so every ring's crest falls out of one
 * formula with no record of which rings are lit.
 *
 * Zero amplitude or wavelength removes lift. A zero period freezes the ripple
 * instead of dividing by zero. */
static inline int32_t
boot_anim_wave_height(int32_t r_q12, uint32_t now_ms, int32_t amp_q12, int32_t wavelength_q12, uint32_t period_ms) {
    if (amp_q12 == 0 || wavelength_q12 <= 0) {
        return 0;
    }

    const uint32_t space_phase = (uint32_t)(((int64_t)r_q12 * 65536) / wavelength_q12);
    const uint32_t time_phase =
        (period_ms == 0) ? 0u : (uint32_t)(((uint64_t)(now_ms % period_ms) * 65536u) / period_ms);

    /* Wraps mod 65536 by uint16_t truncation */
    const uint16_t phase = (uint16_t)(space_phase - time_phase);
    const int32_t sin_q15 = trig_sin(phase);

    const int32_t amp_zeta_q12 = (int32_t)(((int64_t)amp_q12 * sin_q15) >> 15);

    return boot_anim_zeta_to_t_q8(amp_zeta_q12);
}

/* Spline interpolation keeps the curve's tight turns smooth without a larger
 * generated table. */
#define BOOT_ANIM_SPLINE_STEPS          4

#define BOOT_ANIM_LOD_CHORD_PX          3

#define BOOT_ANIM_LOD_STRIDE2_PX        192
#define BOOT_ANIM_LOD_STRIDE4_PX        96

#define BOOT_ANIM_DISSOLVE_HALF_LEVEL   4
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
    /* 32-bit throughout, deliberately not util/fixed.h's widening helpers:
     * the operands are sized so the product cannot overflow an int32, on a
     * path that runs several thousand times a frame. */
    const int32_t u = BOOT_ANIM_ONE - t_q12;
    const int32_t w0 = (u * u) >> BOOT_ANIM_Q;
    const int32_t w2 = (t_q12 * t_q12) >> BOOT_ANIM_Q;

    /* Weights sum to 2.0 by construction, so the middle one is never
     * computed from its own polynomial and cannot drift from the other
     * two. */
    const int32_t w1 = 2 * BOOT_ANIM_ONE - w0 - w2;

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
    const int32_t w1 = 2 * BOOT_ANIM_ONE - w0 - w2;

    /* int64: a camera-space coordinate has no known-small-range promise a raw
     * curve-table value does. */
    return (vec3x_t){
        (int32_t)((((int64_t)w0 * c0.x) + ((int64_t)w1 * c1.x) + ((int64_t)w2 * c2.x)) >> (BOOT_ANIM_Q + 1)),
        (int32_t)((((int64_t)w0 * c0.y) + ((int64_t)w1 * c1.y) + ((int64_t)w2 * c2.y)) >> (BOOT_ANIM_Q + 1)),
        (int32_t)((((int64_t)w0 * c0.z) + ((int64_t)w1 * c1.z) + ((int64_t)w2 * c2.z)) >> (BOOT_ANIM_Q + 1)),
    };
}

/* The chord's screen extent against `px`: the Manhattan length m of the two
 * points, scaled as the projection scales it, compared without a divide. */
static inline bool
boot_anim_screen_chord_lt(vec3x_t a, vec3x_t c, const boot_anim_view_t* view, int32_t px) {
    if (a.z <= view->near_z || c.z <= view->near_z) {
        return false;
    }
    const int64_t m = (int64_t)im_abs(a.x - c.x) + im_abs(a.y - c.y);
    if (view->focal == 0) {
        return m * view->scale < (int64_t)px * R3D_X_UNIT_ONE;
    }
    const int32_t zmin = a.z < c.z ? a.z : c.z;
    return m * view->focal * view->scale < (int64_t)px * zmin * R3D_X_UNIT_ONE;
}

/* Do NOT subdivide if span ends within BOOT_ANIM_LOD_CHORD_PX. Uses
 * boot_anim_screen_chord_lt(). */
static inline int
boot_anim_curve_lod_steps(vec3x_t a, vec3x_t c, const boot_anim_view_t* view) {
    return boot_anim_screen_chord_lt(a, c, view, BOOT_ANIM_LOD_CHORD_PX) ? 1 : BOOT_ANIM_SPLINE_STEPS;
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

/* Every phase uses milliseconds since power-up, so timing is frame-rate
 * independent. */

/* A FRACTION, not a pixel count: arms have different lengths. */
static inline uint8_t
boot_anim_axis_reach(uint32_t now_ms) {
    return tween_ease_out(tween_ramp(now_ms, 0, BOOT_ANIM_AXES_MS));
}

/* Ring fades multiply IN and OUT as a depth cue. Floor is backdrop: full
 * strength competes with the curve and wins, so BOOT_ANIM_GRID_MAX (generated) stays low, not
 * forever (see boot_anim_grid_climb() below), since that holds only
 * while the curve is full-size. One clock drives opacity and whitening
 * together: hue alone fixes muddiness, not peak brightness. Climbs from
 * the floor's appearance to BOOT_ANIM_MS, linear so it reads as steadily
 * adding up. */
static inline uint8_t
boot_anim_grid_climb(uint32_t now_ms) {
    return tween_ramp(now_ms, BOOT_ANIM_GRID_START_MS, BOOT_ANIM_MS - BOOT_ANIM_GRID_START_MS);
}

static inline uint8_t
boot_anim_grid_alpha(uint32_t now_ms, int ring) {
    if (ring >= BOOT_ANIM_GRID_FADE) {
        return 0;
    }
    const uint32_t start = BOOT_ANIM_GRID_START_MS + (uint32_t)ring * BOOT_ANIM_GRID_RING_MS;
    const uint32_t arrived = tween_ramp(now_ms, start, BOOT_ANIM_GRID_FADE_MS);

    /* left/FADE, bounded rings. Squared dims too much. Linear keeps depth. */
    const uint32_t left = (uint32_t)(BOOT_ANIM_GRID_FADE - ring);
    const uint32_t ceiling =
        (uint32_t)tween_lerp_i32(BOOT_ANIM_GRID_MAX, BOOT_ANIM_GRID_CEILING_MAX, boot_anim_grid_climb(now_ms));
    const uint32_t near = left * ceiling / (uint32_t)BOOT_ANIM_GRID_FADE;

    return (uint8_t)((arrived * near) / 255u);
}

static inline uint8_t
boot_anim_grid_spoke_reach(uint32_t now_ms) {
    return tween_ramp(now_ms, BOOT_ANIM_GRID_SPOKE_START_MS, BOOT_ANIM_GRID_SPOKE_DRAW_MS);
}

/* NOT linear in `reach`: a point projects to roughly 1/r on screen, so
 * linear-in-radius puts almost all screen-space growth in the first few
 * percent of the reveal. Interpolating 1/target keeps growth roughly
 * even instead. Below `near`, stays linear-in-radius: 1/target needs a
 * positive radius to anchor to, and 1/0 does not exist. Algebraically:
 * target = (near*far) / (far - (far-near)*frac). Plain math (not
 * boot_anim.c) so it is host-testable; see suite_boot_anim.c. */
static inline int32_t
boot_anim_spoke_reveal_target(int32_t near, int32_t far, uint8_t reach) {
    if (reach == 0) {
        return 0;
    }
    if (reach >= 255 || far <= 0) {
        return far;
    }

    const int32_t r0 = (int32_t)(((int64_t)near * 255) / far);
    if (reach <= r0 || r0 <= 0) {
        return (int32_t)(((int64_t)far * reach) / 255);
    }

    const int32_t frac_q8 = (int32_t)(((int64_t)(reach - r0) << 8) / (255 - r0));
    const int64_t denom_q8 = ((int64_t)far << 8) - (int64_t)(far - near) * frac_q8;
    if (denom_q8 <= 0) {
        return far; /* degenerate: clamp rather than divide by <=0 */
    }
    return (int32_t)((((int64_t)near * far) << 8) / denom_q8);
}

static inline uint8_t
boot_anim_grid_whiten(uint32_t now_ms) {
    return (uint8_t)tween_lerp_i32(0, BOOT_ANIM_GRID_WHITEN_MAX, boot_anim_grid_climb(now_ms));
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
    return (int)((amp * trig_sin(phase)) >> 15);
}

static inline uint8_t
boot_anim_title_wave_reach(uint32_t now_ms) {
    return (uint8_t)(255u - tween_ramp(now_ms, BOOT_ANIM_TITLE_WAVE_OUT_MS, BOOT_ANIM_TITLE_WAVE_FADE_MS));
}

static inline int
boot_anim_title_wave(int i, uint32_t now_ms) {
    const uint32_t t = (now_ms + (uint32_t)i * BOOT_ANIM_TITLE_WAVE_STAGGER_MS) % BOOT_ANIM_TITLE_WAVE_PERIOD_MS;
    const uint16_t phase = (uint16_t)((t * 65536u) / BOOT_ANIM_TITLE_WAVE_PERIOD_MS);
    const int32_t amp = (BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX * boot_anim_title_wave_reach(now_ms)) / 255;
    return (int)((amp * trig_sin(phase)) >> 15);
}

/* Row starts at a FIXED BOOT_ANIM_TITLE_VIEW_X/Y, not read live: the
 * target a letter flies toward has to stay put for the whole flight.
 * final_x sums each letter's real advance via gfx_font_text_width(), not
 * `i * cell_w`: a fixed per-cell reckoning is wrong for a proportional
 * font, where narrow/wide glyphs do not share one width; see
 * test_final_x_matches_the_advance_sum() in suite_boot_anim.c. */
static inline vec2i_t
boot_anim_title_letter(const gfx_font_t* font, int i, uint32_t now_ms) {
    const uint32_t start = BOOT_ANIM_TITLE_START_MS + (uint32_t)i * BOOT_ANIM_TITLE_STAGGER_MS;
    const uint8_t u8 = tween_ease_out(tween_ramp(now_ms, start, BOOT_ANIM_TITLE_FLIGHT_MS));

    const int prefix_w = gfx_font_text_width(font, BOOT_ANIM_TITLE, i, BOOT_ANIM_TITLE_SCALE);
    const int final_x = BOOT_ANIM_TITLE_VIEW_X + prefix_w + i * BOOT_ANIM_TITLE_GAP;
    const int start_x = final_x - BOOT_ANIM_TITLE_ENTRY_PX;

    vec2i_t p;
    p.x = tween_lerp_i32(start_x, final_x, u8);

    const int32_t d_q12 = BOOT_ANIM_ONE - tween_lerp_i32(0, BOOT_ANIM_ONE, u8);
    p.y = BOOT_ANIM_TITLE_VIEW_Y + boot_anim_title_wobble(d_q12) + boot_anim_title_wave(i, now_ms);
    return p;
}

/* Swap and negate offset for shadow. Pure, host-testable function. */
static inline void
boot_anim_title_shadow_offset(int dx, int dy, int* panel_dx, int* panel_dy) {
    *panel_dx = -dy;
    *panel_dy = dx;
}

/* A hue wheel lets height read as colour against the AMOLED's true black.
 * Brightness and desaturation remain separate mixing amounts. */

/* A whole turn: six sectors of 256, so the sector is a shift and the ramp
 * within one is a byte. */
#define BOOT_ANIM_HUE_TURN 1536

/* 0xRRGGBB at full saturation and full brightness. */
static inline uint32_t
boot_anim_hue_rgb(int hue) {
    hue %= BOOT_ANIM_HUE_TURN;
    if (hue < 0) {
        hue += BOOT_ANIM_HUE_TURN;
    }

    const uint32_t ramp = (uint32_t)(hue & 0xFF); /* rising edge, 0..255 */
    const uint32_t fall = 255u - ramp;

    switch (hue >> 8) {
        case 0: return (0xFFu << 16) | (ramp << 8); /* red     -> yellow  */
        case 1: return (fall << 16) | (0xFFu << 8); /* yellow  -> green   */
        case 2: return (0xFFu << 8) | ramp;         /* green   -> cyan    */
        case 3: return (fall << 8) | 0xFFu;         /* cyan    -> blue    */
        case 4: return (ramp << 16) | 0xFFu;        /* blue    -> magenta */
        default: return (0xFFu << 16) | fall;       /* magenta -> red     */
    }
}

static inline int
boot_anim_grid_hue(uint32_t now_ms, int ring) {
    const uint32_t turn = (now_ms % BOOT_ANIM_GRID_HUE_MS) * BOOT_ANIM_HUE_TURN / BOOT_ANIM_GRID_HUE_MS;
    return (int)turn + ring * BOOT_ANIM_GRID_HUE_SPREAD;
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
        const uint8_t linear = tween_ramp(now_ms, BOOT_ANIM_PEN_START_MS, BOOT_ANIM_PEN_MS);
        return tween_lerp_i32(0, BOOT_ANIM_CURVE_PHASE1_FRACTION, linear);
    }

    const uint8_t linear2 = tween_ramp(now_ms, phase1_end_ms, BOOT_ANIM_PEN_FINISH_MS - phase1_end_ms);
    return tween_lerp_i32(BOOT_ANIM_CURVE_PHASE1_FRACTION, BOOT_ANIM_ONE, linear2);
}

static inline int32_t
boot_anim_colour_progress(uint32_t now_ms) {
    const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);
    return (int32_t)(((int64_t)boot_anim_pen(now_ms) * span) / phase1_span);
}

static inline uint8_t
boot_anim_ink(uint32_t now_ms) {
    return (uint8_t)(255u - tween_ramp(now_ms, BOOT_ANIM_FADE_START_MS, BOOT_ANIM_MS - BOOT_ANIM_FADE_START_MS));
}

/* Crossfade to the photograph, 0..255, over BOOT_ANIM_IMAGE_START_MS/
 * FADE_MS. Plain tween_ramp(), not eased: ease_out(r) + ease_out(255-r)
 * is NOT 255 at every r, so easing either half of a cross-dissolve makes
 * the midpoint read brighter than either end; linear is what keeps the
 * two halves summing to one whole picture throughout. */
static inline uint8_t
boot_anim_image_reveal(uint32_t now_ms) {
    return tween_ramp(now_ms, BOOT_ANIM_IMAGE_START_MS, BOOT_ANIM_IMAGE_FADE_MS);
}

/* Complement the image reveal directly: separately rounded ramps need not
 * sum to 255. */
static inline uint8_t
boot_anim_scene_reach(uint32_t now_ms) {
    return (uint8_t)(255u - boot_anim_image_reveal(now_ms));
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
    return pen_q12 - (int32_t)k * BOOT_ANIM_TRAIL_GAP;
}

/* The wheel position pen `k` carries, spread evenly round it. */
static inline int
boot_anim_trail_hue(int k) {
    return k * (BOOT_ANIM_HUE_TURN / BOOT_ANIM_TRAILS);
}

typedef struct {
    int hue;       /* wheel position; boot_anim_hue_rgb() wraps it */
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

/* Axes reach their full extent over the fade interval, so the scene has one
 * ending. */
static inline uint8_t
boot_anim_finale_reach(uint32_t now_ms) {
    return tween_ease_out(tween_ramp(now_ms, BOOT_ANIM_FADE_START_MS, BOOT_ANIM_MS - BOOT_ANIM_FADE_START_MS));
}

/* Axes run past the panel and rely on clipping. Equal to
 * BOOT_ANIM_GRID_SPOKE_FAR_UNITS but deliberately not shared: both need only
 * clear the panel. */
#define BOOT_ANIM_AXIS_FAR_UNITS 500

/* Host render tests call this directly and read the firmware framebuffer. */
void boot_anim_draw_frame(uint32_t now_ms);

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
