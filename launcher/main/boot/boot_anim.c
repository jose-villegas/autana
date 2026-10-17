/*
 * boot_anim: drawing the startup animation, and the five seconds it owns.
 *
 * The projection, the smoothing, the colour and the timeline are all in
 * boot_anim.h, where they are host-testable, and the curve is a generated
 * table in boot_anim_curve.h. What is left here is gfx calls and one loop.
 *
 * THIS LOOP IS NOT THE SHELL'S FRAME LOOP
 *
 * docs/Firmware-Architecture.md says there is exactly one frame loop and it
 * belongs to the shell, which is a rule about APPS: an app must not loop,
 * because the shell has to stay able to switch away from it. Nothing can be
 * switched to yet at this point in boot (touch is not even running), so this
 * runs to completion before app_main() reaches its loop at all, the same way
 * show_post_failures() already blocks on a hardware fault. It still yields
 * every frame, so the idle task keeps feeding the watchdog.
 *
 * EVERY FRAME IS A FULL REPAINT
 *
 * Which is the one thing the rest of this project works hard to avoid; see
 * ui.c on skipping unchanged canvases. It is right there and wrong here: the
 * trail behind the pen re-colours a long stretch of curve every frame, so a
 * frame differs from the one before it almost everywhere and there is nothing
 * to save. The clear also gets the picture back to true black, which is both
 * what the additive strokes need underneath them and (until the photograph
 * arrives; see draw_image()'s own comment on why that phase composites
 * rather than clearing into) what the dissolve at the end fades into.
 */

#include "boot/boot_anim.h"

#include <math.h>
#include <string.h>

#include "build/build_variant.h"
#include "core/timing.h"
#include "display/display.h"
#include "draw/draw.h"
#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/target/gfx_segment_clip.h"
#include "math/scalar/fixed.h"
#include "math/scalar/mathi.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"

static const char* TAG = "boot_anim";
#endif

static boot_anim_backdrop_fn ending_backdrop;

#define COL_BG      GFX_RGB(0x000000)
#define COL_WHITE   GFX_RGB(0xFFFFFF)

#define COL_AXIS    0x5A6478
#define COL_TICK    0x8792A8
#define COL_ZERO    0xFFFFFF

#define LABEL_SCALE 1 /* 8x8 glyphs: an axis label is small */
#define LABEL_GAP   5

#define HEAD_OUTER  13
#define HEAD_MID    8
#define HEAD_CORE   4

/* A zero of zeta, marked on the t axis where the curve crosses it. */
#define ZERO_DOT    5

/* Colour */

/* Folds global dissolve into existing alpha. */
static uint8_t
scale8(uint8_t a, uint8_t b) {
    return (uint8_t)(((uint32_t)a * b + 127u) / 255u);
}

/* `rgb` lifted `alpha` of the way off the background. */
static gfx_color_t
lit(uint32_t rgb, uint8_t alpha) {
    return gfx_color_mix(COL_BG, gfx_rgb(rgb), alpha);
}

/* A whole number of grid units, as a Q12 value. */
static int32_t
units(int n) {
    return (int32_t)n * BOOT_ANIM_ONE;
}

/* NEAR portion vertices span distance, smooth growth, dashed segments, not
 * wave. */
#define BOOT_ANIM_GRID_SPOKE_STEPS      32

/* NEAR spoke reach limit, unrelated to grid size. */
#define BOOT_ANIM_GRID_SPOKE_NEAR_UNITS 10

/* ITS limit is panel edge crossing. Walked as a single extra segment.
 * Clipping handles rest. */
#define BOOT_ANIM_GRID_SPOKE_FAR_UNITS  500

static void
polar_point(int32_t radius, uint16_t turn, int32_t* re, int32_t* im) {
    const int32_t cos_v = trig_cos(turn);
    const int32_t sin_v = trig_sin(turn);
    *re = (int32_t)(((int64_t)radius * cos_v) >> TRIG_SIN_SHIFT);
    *im = (int32_t)(((int64_t)radius * sin_v) >> TRIG_SIN_SHIFT);
}

/* Camera setup scratch must be released before drawing a mesh row. */
__attribute__((noinline)) boot_anim_view_t
boot_anim_view(const boot_anim_motion_t* motion, int w, int h, uint32_t now_ms) {
    boot_anim_timeline_state_t st = boot_anim_timeline_sample(motion, now_ms);

    /* w (the panel's native WIDTH) is narrower than h (its native HEIGHT),
     * so render_view_make() fits the lens to w; boot's pixels must not move
     * if that inequality ever changes. */
    const viewport_t viewport = {.width = w, .height = h, .quarter = 0};
    const render_view_t view =
        render_view_make(&st.camera, BOOT_ANIM_CAMERA_HALF_FOV_SHORT_TAN, R3D_LINE_NEAR_Z, viewport);
    /* Meters per raw unit of (re, t, im): Q12, Q8 climbing 132/512 m a unit, Q12. */
    const float meters_per_unit[3] = {1.0F / (float)BOOT_ANIM_ONE,
                                      (float)BOOT_ANIM_SPIRAL_Q9 / (256.0F * (float)R3D_X_UNIT_ONE),
                                      1.0F / (float)BOOT_ANIM_ONE};
    return r3d_line_view_x_make(&view, &st.space, meters_per_unit);
}

/* What every node and edge of one frame's mesh shares. */
typedef struct {
    uint32_t now_ms;
    boot_anim_floor_state_t floor;
    recti_t clip;
    vec3x_t column;
    bool stepping;
    uint8_t ink;
    int stride;
    const boot_anim_view_t* view;
} mesh_pass_t;

/* A node of the floor mesh, projected. band_q4 is its distance from the
 * origin in BOOT_ANIM_GRID_STEP_Q12 bands, 4 fractional bits. Screen only:
 * draw_floor() keeps a row of these on boot's small stack, and the camera
 * space point an edge needs to clip at the near plane is rebuilt for the few
 * edges that cross it. */
typedef struct {
    int16_t sx, sy;
    int16_t band_q4;
    int16_t ripple;
    uint8_t flags;
} mesh_node_t;

#define MESH_HALF       (BOOT_ANIM_MESH_CELLS / 2)
#define MESH_NODE_FRONT (1u << 4)
#define MESH_NODE_CLIP  (GFX_CLIP_OUT_LEFT | GFX_CLIP_OUT_RIGHT | GFX_CLIP_OUT_TOP | GFX_CLIP_OUT_BOTTOM)

_Static_assert(sizeof(mesh_node_t) == 10, "the mesh row must fit the boot stack");

/* The floor's transform keeps the narrow path's fractional bits until projection. */
static vec3x_t
mesh_point(const mesh_pass_t* pass, int i, int j) {
    const int32_t lift = boot_anim_zeta_to_t_q8(boot_anim_floor_height(i, j, &pass->floor));
    return boot_anim_to_camera_space(i * BOOT_ANIM_MESH_STEP_Q12, j * BOOT_ANIM_MESH_STEP_Q12, lift, pass->view);
}

static vec3x_t
mesh_stepped_point(const mesh_pass_t* pass, int i, int j, vec3x_t plane, int32_t lift) {
    if (!pass->stepping || (uint32_t)(lift + R3D_X_INPUT_Y_LIMIT) >= 2u * R3D_X_INPUT_Y_LIMIT) {
        return boot_anim_to_camera_space(i * BOOT_ANIM_MESH_STEP_Q12, j * BOOT_ANIM_MESH_STEP_Q12, lift, pass->view);
    }
    return (vec3x_t){
        (plane.x + (pass->view->units[0][1] * lift)) >> R3D_X_INPUT_SHIFT,
        (plane.y + (pass->view->units[1][1] * lift)) >> R3D_X_INPUT_SHIFT,
        (plane.z + (pass->view->units[2][1] * lift)) >> R3D_X_INPUT_SHIFT,
    };
}

static mesh_node_t
mesh_node(const mesh_pass_t* pass, int i, int j, vec3x_t plane) {
    int sx = 0;
    int sy = 0;
    mesh_node_t node;
    node.ripple = (int16_t)boot_anim_floor_ripple(i, j, &pass->floor);
    const int32_t lift = boot_anim_zeta_to_t_q8(node.ripple + boot_anim_floor_swell(i, j, &pass->floor));
    const bool front = r3d_project_point_cs_x(mesh_stepped_point(pass, i, j, plane, lift), pass->view, &sx, &sy);
    node.sx = (int16_t)sx;
    node.sy = (int16_t)sy;
    node.band_q4 = (int16_t)boot_anim_floor_node(i, j)->band_q4;
    node.flags = (uint8_t)(gfx_clip_outcode(&pass->clip, sx, sy) | (front ? MESH_NODE_FRONT : 0u));
    return node;
}

#ifndef PROTO_COLOUR_STRIDE
#define PROTO_COLOUR_STRIDE 2
#endif

/* PROTOTYPE (host only): crests lerp from grey toward the colour of the curve
 * sample nearest the node in the floor plane among those the pen has drawn,
 * not toward white. PROTO_COLOUR_STRIDE 2 colours from every other node. */
static int
proto_nearest_drawn(int i, int j, int last) {
    const int64_t re = (int64_t)i * BOOT_ANIM_MESH_STEP_Q12;
    const int64_t im = (int64_t)j * BOOT_ANIM_MESH_STEP_Q12;
    int best = 0;
    int64_t best_d = INT64_MAX;
    for (int k = 0; k <= last && k < BOOT_ANIM_CURVE_POINTS; k++) {
        const int64_t dx = boot_anim_curve[k].re - re;
        const int64_t dy = boot_anim_curve[k].im - im;
        const int64_t d = (dx * dx) + (dy * dy);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

/* PROTOTYPE: 0 drops the dashed floor guides; the mesh lines on them keep
 * their colour. */
static uint8_t proto_axis_arrival(uint32_t now_ms, int band);
#ifndef PROTO_AXIS_LINES_FAR
#define PROTO_AXIS_LINES_FAR 1
#endif

/* PROTOTYPE: the grid's lines (not the axis lines) at this share of their
 * colour, 255 unchanged. */
#ifndef PROTO_GRID_LEVEL
#define PROTO_GRID_LEVEL 179
#endif

#ifndef PROTO_DRAW_GUIDES
#define PROTO_DRAW_GUIDES 0
#endif

#ifndef PROTO_GUIDE_LINES
#define PROTO_GUIDE_LINES 2
#endif

/* PROTOTYPE: mesh lines lying on a guide (row 0 and column 0). 1: their
 * crests lerp toward the guide's colour, not the curve's; 2: the guide's
 * colour whatever their height. */
static gfx_color_t
proto_crest_mix(int32_t ripple, gfx_color_t target) {
    const int32_t crest = (mathi_clamp(ripple, 0, BOOT_ANIM_WHITE_HEIGHT_Q12) * 255) / BOOT_ANIM_WHITE_HEIGHT_Q12;
    return gfx_color_mix(gfx_rgb(BOOT_ANIM_FLOOR_GREY), target, (uint8_t)((crest * crest) / 255));
}

static gfx_color_t
proto_floor_color(const mesh_pass_t* pass, int i, int j, int32_t ripple) {
    const int32_t pen = boot_anim_pen(pass->now_ms);
    const int last = (int)((pen * (int32_t)(BOOT_ANIM_CURVE_POINTS - 1)) >> BOOT_ANIM_Q);
    const int ci = PROTO_COLOUR_STRIDE * (int)lround((double)i / PROTO_COLOUR_STRIDE);
    const int cj = PROTO_COLOUR_STRIDE * (int)lround((double)j / PROTO_COLOUR_STRIDE);
    const int k = proto_nearest_drawn(ci, cj, last);
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);
    const int32_t along = ((k * BOOT_ANIM_ONE) + (phase1_span / 2)) / phase1_span;
    const boot_anim_stroke_t s = boot_anim_stroke(along, boot_anim_colour_progress(pass->now_ms));
    const int32_t crest = (mathi_clamp(ripple, 0, BOOT_ANIM_WHITE_HEIGHT_Q12) * 255) / BOOT_ANIM_WHITE_HEIGHT_Q12;
    const uint8_t amount = (uint8_t)((crest * crest) / 255);
    return gfx_color_mix(gfx_rgb(BOOT_ANIM_FLOOR_GREY), gfx_rgb(gfx_hue_rgb(s.hue)), amount);
}

static void
draw_mesh_edge(const mesh_pass_t* pass, const mesh_node_t* a, int ai, int aj, const mesh_node_t* b, int bi, int bj) {
    const bool a_front = (a->flags & MESH_NODE_FRONT) != 0;
    const bool b_front = (b->flags & MESH_NODE_FRONT) != 0;
    if ((!a_front && !b_front) || (a_front && b_front && (a->flags & b->flags & MESH_NODE_CLIP))) {
        return;
    }
    const int band = (a->band_q4 + b->band_q4) >> (BOOT_ANIM_FLOOR_BAND_SHIFT + 1);
    const bool on_axis = (aj == 0 && bj == 0) || (ai == 0 && bi == 0);
    const uint8_t alpha = scale8(PROTO_AXIS_LINES_FAR && on_axis ? proto_axis_arrival(pass->now_ms, band)
                                                                 : boot_anim_grid_alpha(pass->now_ms, band),
                                 pass->ink);
    if (alpha == 0) {
        return;
    }
    const int32_t ripple = (a->ripple + b->ripple) / 2;
    gfx_color_t floor_c = proto_floor_color(pass, ai, aj, ripple);
    const bool on_guide = (aj == 0 && bj == 0) || (ai == 0 && bi == 0);
    if (PROTO_GUIDE_LINES == 1 && on_guide) {
        floor_c = proto_crest_mix(ripple, gfx_rgb(COL_AXIS));
    } else if (PROTO_GUIDE_LINES == 2 && on_guide) {
        floor_c = gfx_rgb(COL_AXIS);
    } else {
        floor_c = gfx_color_mix(COL_BG, floor_c, PROTO_GRID_LEVEL);
    }
    const gfx_color_t c = gfx_color_mix(COL_BG, floor_c, alpha);
    const unsigned flags = DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX);
    if (a_front && b_front) {
        draw_line_ex(a->sx, a->sy, b->sx, b->sy, c, flags);
        return;
    }
    int x0, y0, x1, y1;
    if (!r3d_project_segment_cs_x(mesh_point(pass, ai, aj), mesh_point(pass, bi, bj), pass->view, &x0, &y0, &x1, &y1)) {
        return;
    }
    draw_line_ex(x0, y0, x1, y1, c, flags);
}

static int
mesh_row_half(int j, int stride) {
    return boot_anim_mesh_row_half[stride - 1][mathi_abs(j)];
}

static void
draw_mesh_row(const mesh_pass_t* pass, int j, int row_half, int above_half, mesh_node_t* above, vec3x_t plane) {
    const int stride = pass->stride;
    mesh_node_t left = {0};
    for (int i = -MESH_HALF; i <= MESH_HALF; i += stride) {
        const vec3x_t point = plane;
        plane = (vec3x_t){plane.x + pass->column.x, plane.y + pass->column.y, plane.z + pass->column.z};
        if (mathi_abs(i) > row_half) {
            continue;
        }
        mesh_node_t node = mesh_node(pass, i, j, point);
        if (i - stride >= -row_half) {
            draw_mesh_edge(pass, &left, i - stride, j, &node, i, j);
        }
        if (mathi_abs(i) <= above_half) {
            draw_mesh_edge(pass, &above[i + MESH_HALF], i, j - stride, &node, i, j);
        }
        above[i + MESH_HALF] = node;
        left = node;
    }
}

#define BOOT_ANIM_GRID_SPOKE_DASH_STEPS 3

/* Drawn FLAT: a spoke is a plane guide line, not the surface the wave
 * moves. TWO PARTS, ONE reveal: `reach` scales one target radius
 * across the full near..far span: the near portion (stepped, dashable)
 * walks to min(target, near); the tail (one segment, undashed) appears
 * once that is done. Stepping the tail at the near resolution wastes
 * budget nothing is close enough to see. `dash` skips whole segment
 * GROUPS (a full draw_line_ex() call each), not pixel-thinning, genuinely
 * cheaper. */
#ifndef PROTO_GUIDE_PIXEL_DASH
#define PROTO_GUIDE_PIXEL_DASH 1
#endif
/* PROTOTYPE: the whole guide dashed by pixels like the mesh, not by groups of
 * segments. */
#define PROTO_GUIDE_FLAGS                                                                                              \
    (PROTO_GUIDE_PIXEL_DASH ? DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX) : 0u)

#ifndef PROTO_GUIDE_EXTEND
#define PROTO_GUIDE_EXTEND 1
#endif

/* PROTOTYPE (host only): a line on the floor ends at its vanishing point on
 * the horizon, mid-screen once the space tilts. Past it, carry the guide on
 * as a screen line to the panel's edge, dashed like the mesh. */
static void
proto_extend_guide(vec3x_t from_cs, int ax, int ay, int bx, int by, bool full, gfx_color_t c,
                   const boot_anim_view_t* view) {
    (void)from_cs;
    (void)view;
    if (!PROTO_GUIDE_EXTEND || !full) {
        return;
    }
    const int dx = bx - ax;
    const int dy = by - ay;
    const int len = mathi_abs(dx) + mathi_abs(dy);
    if (len == 0) {
        return;
    }
    const int far_px = 4 * (GFX_WIDTH + GFX_HEIGHT);
    draw_line_ex(bx, by, bx + (int)(((int64_t)dx * far_px) / len), by + (int)(((int64_t)dy * far_px) / len), c,
                 DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX));
}

static void
draw_grid_spoke(uint16_t turn, int32_t near, int32_t far, gfx_color_t c, bool dash, uint8_t reach,
                const boot_anim_view_t* view) {
    vec3x_t prev_cs;
    bool have_prev = false;
    int32_t last_radius = 0;

    const int32_t target = boot_anim_spoke_reveal_target(near, far, reach);
    const int32_t near_target = target < near ? target : near;
    const boot_anim_plane_t floor_plane = boot_anim_plane(0, view);
    const int max_step = near > 0 ? (int)(((int64_t)BOOT_ANIM_GRID_SPOKE_STEPS * near_target) / near) : 0;

    for (int step = 0; step <= max_step; step++) {
        const int32_t radius = (near * step) / BOOT_ANIM_GRID_SPOKE_STEPS;
        int32_t re, im;
        polar_point(radius, turn, &re, &im);
        const vec3x_t cs = boot_anim_plane_point(&floor_plane, re, im);

        /* Groups of BOOT_ANIM_GRID_SPOKE_DASH_STEPS alternate on and off to
         * form a visible dash. */
        const bool draw_segment =
            PROTO_GUIDE_PIXEL_DASH || !dash || ((step / BOOT_ANIM_GRID_SPOKE_DASH_STEPS) % 2) == 1;
        if (have_prev && draw_segment) {
            int ax, ay, bx, by;
            if (r3d_project_segment_cs_x(prev_cs, cs, view, &ax, &ay, &bx, &by)) {
                draw_line_ex(ax, ay, bx, by, c, PROTO_GUIDE_FLAGS);
            }
        }
        prev_cs = cs;
        have_prev = true;
        last_radius = radius;
    }

    if (have_prev && last_radius < target) {
        int32_t re, im;
        polar_point(target, turn, &re, &im);
        const vec3x_t cs = boot_anim_plane_point(&floor_plane, re, im);
        int ax, ay, bx, by;
        if (r3d_project_segment_cs_x(prev_cs, cs, view, &ax, &ay, &bx, &by)) {
            draw_line_ex(ax, ay, bx, by, c, PROTO_GUIDE_FLAGS);
            proto_extend_guide(prev_cs, ax, ay, bx, by, target >= far, c, view);
        }
    }
}

static void
draw_spokes(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view) {
    const uint8_t spoke_reach = boot_anim_grid_spoke_reach(now_ms);
    if (spoke_reach == 0) {
        return;
    }
    const gfx_color_t spoke_c = lit(COL_AXIS, ink);
    for (int i = 0; i < BOOT_ANIM_GRID_SPOKES; i++) {
        const uint16_t turn = (uint16_t)((i * (int)TRIG_TURN) / BOOT_ANIM_GRID_SPOKES);
        draw_grid_spoke(turn, units(BOOT_ANIM_GRID_SPOKE_NEAR_UNITS), units(BOOT_ANIM_GRID_SPOKE_FAR_UNITS), spoke_c,
                        BOOT_ANIM_GRID_SPOKE_DASH != 0, spoke_reach, view);
    }
}

/* Deltas retain the narrow transform's rounding and input bounds. */
static vec3x_t
mesh_plane(const mesh_pass_t* pass, vec3x_t* row) {
    if (!pass->stepping) {
        *row = (vec3x_t){0};
        return (vec3x_t){0};
    }
    const int32_t start = -MESH_HALF * BOOT_ANIM_MESH_STEP_Q12;
    const int32_t step = pass->stride * BOOT_ANIM_MESH_STEP_Q12;
    const int32_t (*u)[4] = pass->view->units;
    *row = (vec3x_t){u[0][2] * step, u[1][2] * step, u[2][2] * step};
    return (vec3x_t){
        ((u[0][0] + u[0][2]) * start) + u[0][3],
        ((u[1][0] + u[1][2]) * start) + u[1][3],
        ((u[2][0] + u[2][2]) * start) + u[2][3],
    };
}

#ifndef PROTO_AXIS_LINES_FAR
#define PROTO_AXIS_LINES_FAR 1
#endif

/* PROTOTYPE (host only): the mesh lines on the axes carry on past the mesh
 * to the horizon, still lifted by the ripple and the swell, then as a screen
 * line to the panel's edge. The rest of the grid stops at the mesh. */
static uint8_t
proto_axis_arrival(uint32_t now_ms, int band) {
    const uint32_t start = BOOT_ANIM_GRID_START_MS + ((uint32_t)band * BOOT_ANIM_GRID_BAND_MS);
    return (uint8_t)tween_ramp(now_ms, start, BOOT_ANIM_GRID_FADE_MS);
}

static int32_t
proto_height_at(int32_t re_q12, int32_t im_q12, const boot_anim_floor_state_t* state) {
    const double re_m = (double)re_q12 / BOOT_ANIM_ONE;
    const double im_m = (double)im_q12 / BOOT_ANIM_ONE;
    const double r_m = sqrt((re_m * re_m) + (im_m * im_m));
    int32_t ripple = 0;
    const int32_t radius = (int32_t)lround(r_m * BOOT_ANIM_RIPPLE_SHAPE_SCALE);
    const int x = (int)(((uint32_t)mathi_abs(radius - state->radius) * state->inv_width) >> BOOT_ANIM_FLOOR_GAIN_SHIFT);
    if (radius < 65535 && x <= BOOT_ANIM_RIPPLE_GAUSSIAN_EXTENT) {
        int32_t amplitude = (state->amplitude * boot_anim_ripple_gaussian[x]) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
        const uint16_t phase = (uint16_t)((long)lround(r_m * 65536.0 / BOOT_ANIM_RIPPLE_WAVELENGTH_M) & 0xFFFF);
        ripple = (-amplitude * trig_cos((uint16_t)(phase + state->ripple_phase))) >> TRIG_SIN_SHIFT;
    }
    int32_t swell = 0;
    if (state->swell != 0) {
        for (int k = 0; k < BOOT_ANIM_SWELL_COUNT; k++) {
            const boot_anim_swell_setting_t* w = &boot_anim_swell_settings[k];
            const double dir = w->direction_deg * 3.14159265358979 / 180.0;
            const double turns = ((re_m * cos(dir)) + (im_m * sin(dir))) / w->wavelength_m;
            const uint16_t phase = (uint16_t)(((long)lround(turns * 65536.0) & 0xFFFF) + state->phase[k]);
            swell += (boot_anim_swell_weights[k] * trig_sin(phase)) >> TRIG_SIN_SHIFT;
        }
        swell = (swell * state->swell) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
    }
    return ripple + swell;
}

static void
proto_draw_axis_lines_far(const mesh_pass_t* pass) {
    static const int dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    const int32_t edge = MESH_HALF * BOOT_ANIM_MESH_STEP_Q12;
    const int32_t far = units(BOOT_ANIM_GRID_SPOKE_FAR_UNITS);
    const uint8_t alpha = scale8(proto_axis_arrival(pass->now_ms, BOOT_ANIM_GRID_FADE), pass->ink);
    if (alpha == 0) {
        return;
    }
    const gfx_color_t c = gfx_color_mix(COL_BG, gfx_rgb(COL_AXIS), alpha);
    const unsigned flags = DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX);
    for (int d = 0; d < 4; d++) {
        int32_t r = edge;
        vec3x_t prev = {0};
        bool have_prev = false;
        int px = 0, py = 0, qx = 0, qy = 0;
        bool have_screen = false;
        for (;;) {
            const int32_t re = dirs[d][0] * r;
            const int32_t im = dirs[d][1] * r;
            const int32_t lift = boot_anim_zeta_to_t_q8(proto_height_at(re, im, &pass->floor));
            const vec3x_t cs = boot_anim_to_camera_space(re, im, lift, pass->view);
            if (have_prev) {
                int ax, ay, bx, by;
                if (r3d_project_segment_cs_x(prev, cs, pass->view, &ax, &ay, &bx, &by)) {
                    draw_line_ex(ax, ay, bx, by, c, flags);
                    px = ax;
                    py = ay;
                    qx = bx;
                    qy = by;
                    have_screen = true;
                }
            }
            prev = cs;
            have_prev = true;
            if (r >= far) {
                break;
            }
            const int32_t grow = r / 16;
            r += grow > BOOT_ANIM_MESH_STEP_Q12 ? grow : BOOT_ANIM_MESH_STEP_Q12;
            if (r > far) {
                r = far;
            }
        }
        const uint8_t extend = boot_anim_grid_spoke_reach(pass->now_ms);
        if (have_screen && extend > 0) {
            const int dx = qx - px;
            const int dy = qy - py;
            const int len = mathi_abs(dx) + mathi_abs(dy);
            if (len > 0) {
                /* Past the horizon it grows over the guides' old reveal. */
                const int far_px = (4 * (GFX_WIDTH + GFX_HEIGHT) * extend) / 255;
                draw_line_ex(qx, qy, qx + (int)(((int64_t)dx * far_px) / len), qy + (int)(((int64_t)dy * far_px) / len),
                             c, flags);
            }
        }
    }
}

void
draw_floor(uint32_t now_ms, uint8_t ink, uint8_t reveal, const boot_anim_view_t* view) {
    gfx_surface_t surface;
    if (!gfx_draw_begin(&surface)) {
        return;
    }
    mesh_pass_t pass = {
        .now_ms = now_ms,
        .floor = boot_anim_floor_state(now_ms),
        .clip = surface.clip,
        .ink = ink,
        .stride = gfx_dither_level(reveal) >= BOOT_ANIM_DISSOLVE_COARSE_LEVEL ? 2 : 1,
        .view = view,
        .stepping = view->units_ok && ((MESH_HALF + 2) * BOOT_ANIM_MESH_STEP_Q12 < R3D_X_INPUT_XZ_LIMIT),
    };
    if (pass.stepping) {
        const int32_t step = pass.stride * BOOT_ANIM_MESH_STEP_Q12;
        pass.column = (vec3x_t){view->units[0][0] * step, view->units[1][0] * step, view->units[2][0] * step};
    }
    vec3x_t row;
    vec3x_t plane = mesh_plane(&pass, &row);
    mesh_node_t above[BOOT_ANIM_MESH_CELLS + 1];
    int above_half = -1;
    for (int j = -MESH_HALF; j <= MESH_HALF; j += pass.stride) {
        const int row_half = mesh_row_half(j, pass.stride);
        draw_mesh_row(&pass, j, row_half, above_half, above, plane);
        above_half = row_half;
        plane = (vec3x_t){plane.x + row.x, plane.y + row.y, plane.z + row.z};
    }

    if (PROTO_AXIS_LINES_FAR) {
        proto_draw_axis_lines_far(&pass);
    }
    if (PROTO_DRAW_GUIDES) {
        draw_spokes(now_ms, ink, view);
    }
}

/* The axes */

/* Arms arrive together despite differing lengths. */
static void
draw_arm(int32_t re, int32_t im, int32_t t, uint8_t reach, uint8_t ink, const boot_anim_view_t* view) {
    const int32_t fre = mathi_lerp_u8(0, re, reach);
    const int32_t fim = mathi_lerp_u8(0, im, reach);
    const int32_t ft = mathi_lerp_u8(0, t, reach);

    int ax, ay, bx, by;
    if (boot_anim_project_segment(0, 0, 0, fre, fim, ft, view, &ax, &ay, &bx, &by)) {
        draw_line_ex(ax, ay, bx, by, lit(COL_AXIS, ink), 0u);
    }
}

static void
draw_label(int x, int y, const char* text, uint8_t ink) {
    const int w = draw_font_text_width(draw_font_ui(), text, -1, LABEL_SCALE);
    const int h = draw_font_height(draw_font_ui(), LABEL_SCALE);

    draw_text_scaled(x - (w / 2), y - (h / 2), text, lit(COL_TICK, ink), LABEL_SCALE);
}

void
draw_axes(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view) {
    const uint8_t reach = boot_anim_axis_reach(now_ms);
    if (reach == 0) {
        return;
    }

    const uint8_t finale = boot_anim_finale_reach(now_ms);
    const int32_t short_arm = units(4);
    const int32_t long_arm = units(BOOT_ANIM_AXIS_FAR_UNITS);
    const int32_t arm = mathi_lerp_u8(short_arm, long_arm, finale);

    const int32_t short_top = (BOOT_ANIM_T_MAX_PHASE1 + 1) << BOOT_ANIM_TQ;
    const int32_t long_top = boot_anim_zeta_to_t_q8(long_arm);
    const int32_t top = mathi_lerp_u8(short_top, long_top, finale);

    draw_arm(arm, 0, 0, reach, ink, view); /* real      */
    draw_arm(0, arm, 0, reach, ink, view); /* imaginary */
    draw_arm(0, 0, top, reach, ink, view); /* t         */

    if (reach == 255 && finale == 0) {
        /* finale == 0 ensures arms stay in front; consistency with other
         * calls. */
        int re_x, re_y, im_x, im_y, t_x, t_y;
        const bool re_ok = boot_anim_project_point(arm, 0, 0, view, &re_x, &re_y);
        const bool im_ok = boot_anim_project_point(0, arm, 0, view, &im_x, &im_y);
        const bool t_ok = boot_anim_project_point(0, 0, top, view, &t_x, &t_y);

        if (re_ok) {
            draw_label(re_x + (LABEL_GAP * 2), re_y + LABEL_GAP, "Re", ink);
        }
        if (im_ok) {
            draw_label(im_x - (LABEL_GAP * 2), im_y + LABEL_GAP, "Im", ink);
        }
        if (t_ok) {
            draw_label(t_x + (LABEL_GAP * 2), t_y - LABEL_GAP, "t", ink);
        }
    }
}

/* The zeros */

void
draw_zeros(int32_t pen_t_q8, uint8_t ink, const boot_anim_view_t* view) {
    for (int i = 0; i < BOOT_ANIM_ZEROS; i++) {
        const int32_t t = boot_anim_zero_t[i];
        if (t > pen_t_q8) {
            break; /* the table is in order, so nothing after it either */
        }
        int x, y;
        if (!boot_anim_project_point(0, 0, t, view, &x, &y)) {
            continue;
        }
        draw_rect(x - (ZERO_DOT / 2), y - (ZERO_DOT / 2), ZERO_DOT, ZERO_DOT, lit(COL_ZERO, ink));
    }
}

/* The curve */

static void
draw_stroke(int x0, int y0, int x1, int y1, gfx_color_t c, int width, bool joined) {
    /* Offsets spread to thicken curve centrally. */
    const int half = width / 2;
    const bool shallow = mathi_abs(x1 - x0) > mathi_abs(y1 - y0);

    for (int i = 0; i < width; i++) {
        const int off = i - half;
        const int ax = shallow ? x0 : x0 + off;
        const int ay = shallow ? y0 + off : y0;
        const int bx = shallow ? x1 : x1 + off;
        const int by = shallow ? y1 + off : y1;

        draw_line_ex(ax, ay, bx, by, c, DRAW_LINE_ADD | (joined ? DRAW_LINE_OPEN : 0u));
    }
}

static void
draw_disc(int x, int y, int diameter, gfx_color_t c) {
    const int r = diameter / 2;

    for (int dy = -r; dy <= r; dy++) {
        int dx = r;
        while (dx > 0 && (dx * dx) + (dy * dy) > r * r) {
            dx--;
        }
        draw_rect(x - dx, y + dy, (2 * dx) + 1, 1, c);
    }
}

static void
draw_head(int x, int y, uint32_t rgb, uint8_t ink) {
    draw_disc(x, y, HEAD_OUTER, lit(rgb, scale8(70, ink)));
    draw_disc(x, y, HEAD_MID, lit(rgb, scale8(215, ink)));
    draw_disc(x, y, HEAD_CORE, gfx_color_mix(COL_BG, COL_WHITE, ink));
}

static void
draw_heads(int32_t colour_pen, uint8_t ink, const boot_anim_view_t* view) {
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);

    for (int k = 0; k < BOOT_ANIM_TRAILS; k++) {
        const int32_t at = boot_anim_trail_pos(colour_pen, k);
        if (at <= 0) {
            continue; /* not set off yet */
        }

        int i = fx_mul_floor(at, phase1_span, BOOT_ANIM_Q);
        if (i >= BOOT_ANIM_CURVE_POINTS) {
            i = BOOT_ANIM_CURVE_POINTS - 1; /* the table ran out first */
        }
        const boot_anim_pt_t p = boot_anim_sample(i);
        int x, y;
        if (!boot_anim_project_point(p.re, p.im, p.t, view, &x, &y)) {
            continue;
        }

        draw_head(x, y, gfx_hue_rgb(boot_anim_stroke(at, colour_pen).hue), ink);
    }
}

typedef struct {
    vec3x_t prev_cs;
    bool prev_front;
    int prev_sx, prev_sy;
    bool joined;
} curve_segment_t;

static void
draw_curve_segment(curve_segment_t* segment, vec3x_t next_cs, gfx_color_t color, int width,
                   const boot_anim_view_t* view) {
    int nsx, nsy;
    const bool next_front = r3d_project_point_cs_x(next_cs, view, &nsx, &nsy);

    if (segment->prev_front && next_front) {
        draw_stroke(segment->prev_sx, segment->prev_sy, nsx, nsy, color, width, segment->joined);
        segment->joined = true;
        segment->prev_sx = nsx;
        segment->prev_sy = nsy;
    } else if (segment->prev_front != next_front) {
        int ax, ay, bx, by;
        if (r3d_project_segment_cs_x(segment->prev_cs, next_cs, view, &ax, &ay, &bx, &by)) {
            draw_stroke(ax, ay, bx, by, color, width, segment->joined);
            segment->joined = true;
            if (next_front) {
                segment->prev_sx = bx;
                segment->prev_sy = by;
            }
        } else {
            segment->joined = false;
        }
    } else {
        segment->joined = false;
    }
    segment->prev_cs = next_cs;
    segment->prev_front = next_front;
}

/* Re-colours within half the curve's length of the head. No second
 * framebuffer available. */
int32_t
draw_curve(uint32_t now_ms, uint8_t ink, uint8_t reveal, const boot_anim_view_t* view) {
    const int32_t pen = boot_anim_pen(now_ms);
    if (pen <= 0) {
        return 0;
    }

    const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
    const int32_t at = pen * span;
    const int last = at >> BOOT_ANIM_Q;
    const int32_t part = at & (BOOT_ANIM_ONE - 1);

    const int32_t colour = boot_anim_colour_progress(now_ms);
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);

    const int stride = boot_anim_curve_stride(view);

    /* BOOT_ANIM_DISSOLVE_HALF_LEVEL halves; COARSE_LEVEL collapses to chord.
     * Quadratic limits deviation. */
    const int dissolve_level = gfx_dither_level(reveal);
    const int max_steps = dissolve_level >= BOOT_ANIM_DISSOLVE_COARSE_LEVEL ? 1
                          : dissolve_level >= BOOT_ANIM_DISSOLVE_HALF_LEVEL ? 2
                                                                            : BOOT_ANIM_SPLINE_STEPS;

    boot_anim_pt_t s0 = boot_anim_sample(-stride);
    boot_anim_pt_t s1 = boot_anim_sample(0);
    vec3x_t ta = boot_anim_key_to_camera_space(&s0, view);
    vec3x_t tb = boot_anim_key_to_camera_space(&s1, view);
    curve_segment_t segment = {.prev_cs = tb};
    segment.prev_front = r3d_project_point_cs_x(tb, view, &segment.prev_sx, &segment.prev_sy);

    int32_t a0 = 0;

    for (int i = 0; i <= last && i < BOOT_ANIM_CURVE_POINTS; i += stride) {
        const boot_anim_pt_t s2 = boot_anim_sample(i + stride);
        const vec3x_t tc = boot_anim_key_to_camera_space(&s2, view);

        /* Interpolate colour across spans, not per sample. */
        const int32_t a1 = ((i + stride) * BOOT_ANIM_ONE + phase1_span / 2) / phase1_span;

        /* Head jumps from sample to sample without partial span. */
        const int32_t limit = (stride == 1 && i == last) ? part : BOOT_ANIM_ONE;

        /* See boot_anim_curve_lod_steps(). Dissolve cap limits. */
        int steps = boot_anim_curve_lod_steps(ta, tc, view);
        if (steps > max_steps) {
            steps = max_steps;
        }

        const boot_anim_stroke_t s = boot_anim_stroke(a0 + ((a1 - a0) >> 1), colour);
        gfx_color_t span_c = gfx_rgb(gfx_hue_rgb(s.hue));
        span_c = gfx_color_mix(span_c, COL_WHITE, s.bloom);
        span_c = gfx_color_mix(COL_BG, span_c, scale8(s.glow, ink));

        for (int step = 1; step <= steps; step++) {
            const int32_t t = (limit * step) / steps;

            const vec3x_t next_cs = boot_anim_spline_cs(ta, tb, tc, t);
            draw_curve_segment(&segment, next_cs, span_c, s.width, view);
        }

        a0 = a1;
        ta = tb;
        tb = tc;
    }

    draw_heads(colour, ink, view);

    const boot_anim_pt_t final_c0 = boot_anim_sample(last - 1);
    const boot_anim_pt_t final_c1 = boot_anim_sample(last);
    const boot_anim_pt_t final_c2 = boot_anim_sample(last + 1);
    return boot_anim_spline(final_c0, final_c1, final_c2, part).t;
}

/* The title */

static void
title_glyph_origin(int view_x, int view_y, int glyph_w, int glyph_h, int* panel_x, int* panel_y) {
    (void)glyph_w; /* DISPLAY_LANDSCAPE uses box height; see mapped corners. */
    *panel_x = GFX_WIDTH - view_y - glyph_h;
    *panel_y = view_x;
}

/* White, not hue-wheel: stays legible by not competing on colour.
 * Drop-shadow dx/dy (authored, signed) go through
 * boot_anim_title_shadow_offset()'s quarter-turn first: "down-right" in
 * the reader's frame differs from panel space. DITHERED, not solid: fake
 * transparency, this panel's usual trick with no real blending; 255 is
 * pixel-identical to a solid draw, 0 disables. Halo is plain COL_BG, not
 * luminance-derived: that flips white the instant ink dips dark,
 * backwards for fading to black. */
void
draw_title(uint32_t now_ms, uint8_t ink) {
    /* Over black a title fades by darkening. Over the screen that follows,
     * darkening would leave black letters on it, so it dithers away at full
     * colour, the same way the photograph under it does. */
    const bool dissolve = ending_backdrop != NULL && ink < 255;
    const gfx_color_t c = dissolve ? COL_WHITE : gfx_color_mix(COL_BG, COL_WHITE, ink);
    const draw_font_t* font = draw_font_ui();
    const int glyph_w = draw_font_text_width(font, "A", -1, BOOT_ANIM_TITLE_SCALE);
    const int glyph_h = draw_font_height(font, BOOT_ANIM_TITLE_SCALE);
    char one[2] = {0, 0};

    const bool has_shadow =
        (BOOT_ANIM_TITLE_SHADOW_DX != 0 || BOOT_ANIM_TITLE_SHADOW_DY != 0) && BOOT_ANIM_TITLE_SHADOW_ALPHA != 0;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const vec2i_t p = boot_anim_title_letter(font, i, now_ms);
        int px, py;
        title_glyph_origin(p.x, p.y, glyph_w, glyph_h, &px, &py);

        one[0] = BOOT_ANIM_TITLE[i];
        if (has_shadow) {
            int shadow_dx, shadow_dy;
            boot_anim_title_shadow_offset(BOOT_ANIM_TITLE_SHADOW_DX, BOOT_ANIM_TITLE_SHADOW_DY, &shadow_dx, &shadow_dy);
            const uint8_t shadow_alpha =
                dissolve ? (uint8_t)(BOOT_ANIM_TITLE_SHADOW_ALPHA * ink / 255) : BOOT_ANIM_TITLE_SHADOW_ALPHA;
            draw_text_font_dither(px + shadow_dx, py + shadow_dy, one, COL_BG, BOOT_ANIM_TITLE_SCALE, DISPLAY_LANDSCAPE,
                                  font, shadow_alpha);
        }
        if (dissolve) {
            draw_text_font_dither(px, py, one, c, BOOT_ANIM_TITLE_SCALE, DISPLAY_LANDSCAPE, font, ink);
        } else {
            draw_text_font(px, py, one, c, BOOT_ANIM_TITLE_SCALE, DISPLAY_LANDSCAPE, font);
        }
    }
}

/*
 * The one thing here that is not drawn but COMPOSITED. Every draw_* call
 * above stores a colour without reading the framebuffer, which is fine over
 * black and wrong over a photograph: a half-faded grid line drawn that way
 * paints an opaque scratch across the mountain. So the crossfade is a choice
 * not to WRITE: gfx_dither_covers() inspects only a pixel's (x, y), and the
 * scene stays as drawn wherever the reveal does not yet cover it.
 */
void
draw_image(const draw_image_t* photo, uint8_t ink, uint8_t reveal) {
    if (reveal == 0) {
        return;
    }

    /* DITHERED, not blended: no framebuffer-read blend hardware, the same
     * trade this file makes for the title's shadow (draw_title()). A
     * per-pixel gfx_color_mix() over every pixel was measurably the most
     * expensive part of a crossfade frame; see suite_boot_anim_perf.c. */
    draw_blit_dither(0, 0, GFX_WIDTH, GFX_HEIGHT, photo->pixels, (int)photo->stride, reveal < ink ? reveal : ink);
}

/* The loop */

/* EVERY FRAME IS A FULL REPAINT suite_boot_anim_perf.c times this phase
 * draw_clear() cost in draw.c */
void
boot_anim_clear_frame(void) {
    draw_clear(COL_BG);
}

void
boot_anim_set_ending_backdrop(boot_anim_backdrop_fn paint) {
    ending_backdrop = paint;
}

/* Keep scene scratch off the stack while the ending backdrop repaints. */
static __attribute__((noinline)) void
draw_scene(const boot_anim_motion_t* motion, const draw_image_t* photo, uint32_t now_ms, uint8_t ink) {
    const uint8_t reveal = photo->pixels != NULL ? boot_anim_photo_reveal(now_ms) : 0;
    const boot_anim_view_t view = boot_anim_view(motion, GFX_WIDTH, GFX_HEIGHT, now_ms);

    /* A photograph fully revealed covers the whole scene. */
    if (reveal < 255) {
        draw_floor(now_ms, ink, reveal, &view);
        draw_axes(now_ms, ink, &view);

        const int32_t reached = draw_curve(now_ms, ink, reveal, &view);
        draw_zeros(reached, ink, &view);
    }

    draw_image(photo, ink, reveal);

    /* Gated to save gfx calls before BOOT_ANIM_TITLE_START_MS. */
    if (now_ms >= BOOT_ANIM_TITLE_START_MS) {
        draw_title(now_ms, ink);
    }
}

void
boot_anim_draw_frame(const boot_anim_motion_t* motion, const draw_image_t* photo, uint32_t now_ms) {
    const uint8_t ink = boot_anim_ink(now_ms);
    if (ending_backdrop != NULL && ink < 255) {
        ending_backdrop();
    } else {
        boot_anim_clear_frame();
    }
    draw_scene(motion, photo, now_ms, ink);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
report_fps_windowed(int64_t now_us, uint32_t now_ms, int64_t* window_start, uint32_t* frames) {
    (*frames)++;
    const int64_t since = now_us - *window_start;
    if (since >= 500000) {
        ESP_LOGI(TAG, "t=%ums: %.1f fps", (unsigned)now_ms, (double)*frames * 1000000.0 / (double)since);
        *frames = 0;
        *window_start = now_us;
    }
}
#endif

#ifdef ESP_PLATFORM
void
boot_anim_run(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t entered_us = timing_now_us();
    int64_t first_frame_us = 0;
    int64_t photo_mount_us = 0;
    int64_t photo_frame_us = 0;
#endif
    boot_anim_motion_t motion;
    boot_anim_motion_load(&motion);
    draw_image_t photo = {0};
    bool photo_tried = false;
    const int64_t started_us = timing_now_us();
    uint32_t frames = 0;
#if CONFIG_LAUNCHER_DEVELOPMENT
    int64_t fps_window_start = started_us;
    uint32_t fps_window_frames = 0;
#endif

    for (;;) {
        const int64_t now_us = timing_now_us();
        const int64_t elapsed_us = now_us - started_us;
        const uint32_t now_ms = (uint32_t)(elapsed_us / 1000);
        if (now_ms >= BOOT_ANIM_MS) {
            break;
        }

        const bool mounts = !photo_tried && boot_anim_photo_due(frames);
        if (mounts) {
            boot_anim_photo_load(&photo);
            photo_tried = true;
#if CONFIG_LAUNCHER_DEVELOPMENT
            photo_mount_us = timing_now_us() - now_us;
#endif
        }
        boot_anim_draw_frame(&motion, &photo, now_ms);
        gfx_present();
#if CONFIG_LAUNCHER_DEVELOPMENT
        if (frames == 0) {
            first_frame_us = timing_now_us() - entered_us;
        }
        if (mounts) {
            photo_frame_us = timing_now_us() - now_us;
        }
        report_fps_windowed(now_us, now_ms, &fps_window_start, &fps_window_frames);
#endif
        frames++;

        /* The same yield the shell's loop makes, for the same reason: the
         * idle task feeds the watchdog. */
        timing_yield();
    }
    /* Boot plays once and nothing else reads its clip: releasing gives back the
     * pack and its mount slot. */
    boot_anim_motion_release(&motion);
    boot_anim_photo_release(&photo);

    /* Checked only on the board; not on host. */
    ESP_LOGI(TAG, "%u frames in %d ms (%.1f fps)", (unsigned)frames, BOOT_ANIM_MS,
             (double)frames * 1000.0 / BOOT_ANIM_MS);
#if CONFIG_LAUNCHER_DEVELOPMENT
    ESP_LOGI(TAG, "first frame %lld us; photograph's frame %lld us, %lld of them mounting it",
             (long long)first_frame_us, (long long)photo_frame_us, (long long)photo_mount_us);
#endif
}
#endif
