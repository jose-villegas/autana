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

#include <string.h>

#include "build/build_variant.h"
#include "core/timing.h"
#include "display/display.h"
#include "draw/draw.h"
#include "draw/draw_font_roles.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
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
    return (uint8_t)(((uint32_t)a * b + 127U) / 255U);
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
    return r3d_line_projector_make(&view, &st.space, meters_per_unit);
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
    int drawn;          /* the last curve sample the pen has drawn */
    int32_t colour_pen; /* the curve's colour progress */
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
    if (!pass->stepping || (uint32_t)(lift + R3D_X_INPUT_Y_LIMIT) >= 2U * R3D_X_INPUT_Y_LIMIT) {
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
    node.flags = (uint8_t)(gfx_clip_outcode(&pass->clip, sx, sy) | (front ? MESH_NODE_FRONT : 0U));
    return node;
}

/* Below the crest height's first step the edge is the floor's grey, so
 * only crests pay for the curve's colour. */
static gfx_color_t
mesh_edge_color(const mesh_pass_t* pass, int i, int j, int32_t ripple, bool on_axis) {
    if (on_axis) {
        return gfx_rgb(COL_AXIS);
    }
    if (boot_anim_crest_amount(ripple) == 0) {
        return gfx_rgb(BOOT_ANIM_FLOOR_GREY);
    }
    const int sample = boot_anim_crest_sample(i, j, pass->drawn);
    return boot_anim_floor_color(ripple, boot_anim_crest_rgb(sample, pass->colour_pen));
}

static void
draw_mesh_edge(const mesh_pass_t* pass, const mesh_node_t* a, int ai, int aj, const mesh_node_t* b, int bi, int bj) {
    const bool a_front = (a->flags & MESH_NODE_FRONT) != 0;
    const bool b_front = (b->flags & MESH_NODE_FRONT) != 0;
    if ((!a_front && !b_front) || (a_front && b_front && (a->flags & b->flags & MESH_NODE_CLIP))) {
        return;
    }
    const int band = (a->band_q4 + b->band_q4) >> (BOOT_ANIM_FLOOR_BAND_SHIFT + 1);
    /* The lines on the axes carry the axes' flat colour, not the crests'. */
    const bool on_axis = (aj == 0 && bj == 0) || (ai == 0 && bi == 0);
    /* The axis lines run on past the mesh, so they do not fade at its edge. */
    const uint8_t fade =
        on_axis ? boot_anim_grid_arrival(pass->now_ms, band) : boot_anim_grid_alpha(pass->now_ms, band);
    const uint8_t alpha = scale8(scale8(fade, on_axis ? UINT8_MAX : BOOT_ANIM_GRID_LEVEL), pass->ink);
    if (alpha == 0) {
        return;
    }
    const gfx_color_t c =
        gfx_color_mix(COL_BG, mesh_edge_color(pass, ai, aj, (a->ripple + b->ripple) / 2, on_axis), alpha);
    const unsigned flags = DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX);
    if (a_front && b_front) {
        draw_line_ex(a->sx, a->sy, b->sx, b->sy, c, flags);
        return;
    }
    int x0;
    int y0;
    int x1;
    int y1;
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

static const int axis_dirs[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

/* The point of axis sample `n` out along `dir`, in camera space, lifted. */
static vec3x_t
axis_far_point(const mesh_pass_t* pass, const int dir[2], int n) {
    const int32_t r = boot_anim_axis_far[n].r;
    const int32_t lift = boot_anim_zeta_to_t_q8(boot_anim_axis_far_height(n, dir[0], dir[1], &pass->floor));
    return boot_anim_to_camera_space(dir[0] * r, dir[1] * r, lift, pass->view);
}

/* Past the mesh only the lines on the axes go on, still lifted by the ripple
 * and the swell, out to the floor's reach; the last span is cut there. */
static void
draw_axis_lines_far(const mesh_pass_t* pass) {
    const int32_t reach = pass->floor.far_reach;
    if (reach <= boot_anim_axis_far[0].r) {
        return;
    }
    const gfx_color_t c = lit(COL_AXIS, pass->ink);
    const unsigned flags = DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX);
    for (int d = 0; d < 4; d++) {
        vec3x_t prev = axis_far_point(pass, axis_dirs[d], 0);
        for (int n = 1; n < BOOT_ANIM_AXIS_FAR_SAMPLES; n++) {
            vec3x_t next = axis_far_point(pass, axis_dirs[d], n);
            const int32_t r0 = boot_anim_axis_far[n - 1].r;
            const int32_t r1 = boot_anim_axis_far[n].r;
            const bool last = r1 >= reach;
            if (last && r1 > reach) {
                const int64_t part = reach - r0;
                const int64_t span = r1 - r0;
                next = (vec3x_t){prev.x + (int32_t)(((int64_t)(next.x - prev.x) * part) / span),
                                 prev.y + (int32_t)(((int64_t)(next.y - prev.y) * part) / span),
                                 prev.z + (int32_t)(((int64_t)(next.z - prev.z) * part) / span)};
            }
            int x0;
            int y0;
            int x1;
            int y1;
            if (r3d_project_segment_cs_x(prev, next, pass->view, &x0, &y0, &x1, &y1)) {
                draw_line_ex(x0, y0, x1, y1, c, flags);
            }
            if (last) {
                break;
            }
            prev = next;
        }
    }
}

/* One far line, the one through `c` along re (along_im false) or im,
 * between the circles of radius r_in and r_out, both halves; inside the
 * mesh's own disc the mesh draws instead. */
static void
draw_far_ring(const mesh_pass_t* pass, const boot_anim_plane_t* plane, bool along_im, int32_t c, int32_t r_in,
              int32_t r_out, gfx_color_t col) {
    const int32_t edge = boot_anim_axis_far[0].r;
    const int32_t inner = boot_anim_far_chord(mathi_max(r_in, edge), c);
    const int32_t outer = boot_anim_far_chord(r_out, c);
    if (outer <= inner) {
        return;
    }
    const unsigned flags = DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX);
    for (int half = -1; half <= 1; half += 2) {
        const int32_t from = half * inner;
        const int32_t to = half * outer;
        /* Flat, so the plane's two-term form gives the camera point. */
        const vec3x_t a = along_im ? boot_anim_plane_point(plane, c, from) : boot_anim_plane_point(plane, from, c);
        const vec3x_t b = along_im ? boot_anim_plane_point(plane, c, to) : boot_anim_plane_point(plane, to, c);
        int x0;
        int y0;
        int x1;
        int y1;
        if (r3d_project_segment_cs_x(a, b, pass->view, &x0, &y0, &x1, &y1)) {
            draw_line_ex(x0, y0, x1, y1, col, flags);
        }
    }
}

/* A far line's sample, lifted by the swell: its camera-space point, kept
 * for the near-plane clip, and its projection; see mesh_node(). */
typedef struct {
    vec3x_t point;
    mesh_node_t node;
} far_swell_node_t;

static far_swell_node_t
far_swell_node(const mesh_pass_t* pass, const boot_anim_far_line_t* line, int n, bool along_im, int side, int half) {
    const int32_t c = side * line->c;
    const int32_t s = half * boot_anim_far_samples[line->first + n].s;
    const int32_t lift = boot_anim_zeta_to_t_q8(boot_anim_far_line_swell(line, n, along_im, side, half, &pass->floor));
    far_swell_node_t at = {.point = along_im ? boot_anim_to_camera_space(c, s, lift, pass->view)
                                             : boot_anim_to_camera_space(s, c, lift, pass->view)};
    int sx = 0;
    int sy = 0;
    const bool front = r3d_project_point_cs_x(at.point, pass->view, &sx, &sy);
    at.node.sx = (int16_t)sx;
    at.node.sy = (int16_t)sy;
    at.node.flags = (uint8_t)(gfx_clip_outcode(&pass->clip, sx, sy) | (front ? MESH_NODE_FRONT : 0U));
    return at;
}

/* r3d_camera_to_screen_x() truncates x / z to 1/512 and then the pixel, both
 * toward the centre: well under two pixels at boot's lens. */
#define FAR_CULL_MARGIN_PX 2

/* Which sides of the view a camera-space point is past, FAR_CULL_MARGIN_PX
 * beyond the clip, as linear tests: the near plane and the four planes
 * through the camera and the clip's edges. */
static unsigned
far_view_outcode(const mesh_pass_t* pass, vec3x_t p) {
    const projectx_t* lens = &pass->view->lens;
    const int64_t x = (int64_t)p.x * lens->fx;
    const int64_t y = (int64_t)p.y * lens->fy;
    const int64_t left = ((int64_t)(pass->clip.x - FAR_CULL_MARGIN_PX) * MATHX_ONE) - lens->cx;
    const int64_t right = ((int64_t)(recti_right(pass->clip) + FAR_CULL_MARGIN_PX) * MATHX_ONE) - lens->cx;
    const int64_t top = lens->cy - ((int64_t)(pass->clip.y - FAR_CULL_MARGIN_PX) * MATHX_ONE);
    const int64_t bottom = lens->cy - ((int64_t)(recti_bottom(pass->clip) + FAR_CULL_MARGIN_PX) * MATHX_ONE);
    unsigned code = p.z <= r3d_line_near_z(pass->view) ? MESH_NODE_FRONT : 0U;
    code |= x < left * p.z ? GFX_CLIP_OUT_LEFT : 0U;
    code |= x > right * p.z ? GFX_CLIP_OUT_RIGHT : 0U;
    code |= y > top * p.z ? GFX_CLIP_OUT_TOP : 0U;
    code |= y < bottom * p.z ? GFX_CLIP_OUT_BOTTOM : 0U;
    return code;
}

/* Whether a far line's half lies wholly past one side of the view. Each of
 * its samples is a point of the flat chord lifted at most `lift` either way,
 * so all of them lie in the parallelogram of the chord's ends at +-lift; the
 * view's sides are planes, so its four corners decide. */
static bool
far_half_outside(const mesh_pass_t* pass, const boot_anim_far_line_t* line, bool along_im, int side, int half,
                 int32_t lift) {
    const boot_anim_far_sample_t* run = &boot_anim_far_samples[line->first];
    const int32_t c = side * line->c;
    unsigned all = ~0U;
    for (int corner = 0; corner < 4 && all != 0; corner++) {
        const int32_t s = half * run[(corner & 1) != 0 ? line->count - 1 : 0].s;
        const int32_t t = (corner & 2) != 0 ? -lift : lift;
        const vec3x_t p =
            along_im ? boot_anim_to_camera_space(c, s, t, pass->view) : boot_anim_to_camera_space(s, c, t, pass->view);
        all &= far_view_outcode(pass, p);
    }
    return all != 0;
}

/* One span of a lifted far line, from its projected ends where both are in
 * front, else clipped at the near plane; see draw_mesh_edge(). */
static void
draw_far_swell_span(const mesh_pass_t* pass, const far_swell_node_t* a, const far_swell_node_t* b, gfx_color_t col) {
    const unsigned flags = DRAW_LINE_DASH(BOOT_ANIM_MESH_DASH_ON_PX, BOOT_ANIM_MESH_DASH_OFF_PX);
    const uint8_t both = a->node.flags & b->node.flags;
    if ((both & MESH_NODE_FRONT) != 0) {
        if ((both & MESH_NODE_CLIP) == 0) {
            draw_line_ex(a->node.sx, a->node.sy, b->node.sx, b->node.sy, col, flags);
        }
        return;
    }
    int x0;
    int y0;
    int x1;
    int y1;
    if (((a->node.flags | b->node.flags) & MESH_NODE_FRONT) != 0
        && r3d_project_segment_cs_x(a->point, b->point, pass->view, &x0, &y0, &x1, &y1)) {
        draw_line_ex(x0, y0, x1, y1, col, flags);
    }
}

/* Where the swell still lifts it, a far line is a polyline through its baked
 * samples, each projected once; draw_far_ring() draws the flat rest. `lift`
 * bounds the swell's height this frame, in t. */
static void
draw_far_swell_line(const mesh_pass_t* pass, const boot_anim_far_line_t* line, bool along_im, int side, int32_t lift,
                    gfx_color_t col) {
    for (int half = -1; half <= 1; half += 2) {
        if (far_half_outside(pass, line, along_im, side, half, lift)) {
            continue;
        }
        far_swell_node_t prev = far_swell_node(pass, line, 0, along_im, side, half);
        for (int n = 1; n < line->count; n++) {
            const far_swell_node_t next = far_swell_node(pass, line, n, along_im, side, half);
            draw_far_swell_span(pass, &prev, &next, col);
            prev = next;
        }
    }
}

/* One far line's front, the `front` past `solid`, fading out over
 * BOOT_ANIM_FAR_FRONT_STEPS rings while the reach grows. */
static void
draw_far_front(const mesh_pass_t* pass, const boot_anim_plane_t* plane, bool along_im, int32_t c, int32_t solid,
               int32_t front, uint8_t level) {
    for (int k = 0; front > 0 && k < BOOT_ANIM_FAR_FRONT_STEPS; k++) {
        const int32_t r_in = solid + (int32_t)(((int64_t)front * k) / BOOT_ANIM_FAR_FRONT_STEPS);
        const int32_t r_out = solid + (int32_t)(((int64_t)front * (k + 1)) / BOOT_ANIM_FAR_FRONT_STEPS);
        const uint8_t fade = (uint8_t)((UINT8_MAX * (BOOT_ANIM_FAR_FRONT_STEPS - k)) / (BOOT_ANIM_FAR_FRONT_STEPS + 1));
        draw_far_ring(pass, plane, along_im, c, r_in, r_out, lit(BOOT_ANIM_FLOOR_GREY, scale8(level, fade)));
    }
}

/* The plane past the mesh: grid lines out to the floor's reach, cut to its
 * circle so it arrives radially. Its outer BOOT_ANIM_FAR_FRONT_Q8 share fades
 * in over BOOT_ANIM_FAR_FRONT_STEPS rings until the reach is complete. Once
 * the swell starts the reach is whole, and the swell lifts each line out to
 * BOOT_ANIM_FAR_SWELL_FADE_Q12 until it eases out before the crossfade; the
 * rest stays flat and straight. The lines on the axes are drawn on their own. */
static void
draw_far_grid(const mesh_pass_t* pass) {
    const int32_t reach = pass->floor.far_reach;
    if (reach <= boot_anim_axis_far[0].r) {
        return;
    }
    const uint8_t level = scale8(BOOT_ANIM_GRID_LEVEL, pass->ink);
    const int32_t front = reach >= BOOT_ANIM_FAR_Q12 ? 0 : (int32_t)(((int64_t)reach * BOOT_ANIM_FAR_FRONT_Q8) >> 8);
    const int32_t solid = reach - front;
    const boot_anim_plane_t plane = boot_anim_plane(0, pass->view);
    const gfx_color_t col = lit(BOOT_ANIM_FLOOR_GREY, level);
    /* One more than the far swell's height, for zeta_to_t's rounding. */
    const int32_t bound = (boot_anim_swell_bound(&pass->floor) * pass->floor.far_swell) >> BOOT_ANIM_FLOOR_GAIN_SHIFT;
    const int32_t lift = boot_anim_zeta_to_t_q8(bound) + 1;
    /* Four ways per line, along re or im on either side of the axis. */
    for (int way = 0; way < 4 * BOOT_ANIM_FAR_LINES && boot_anim_far_lines[way / 4].c <= reach; way++) {
        const boot_anim_far_line_t* far_line = &boot_anim_far_lines[way / 4];
        const bool along_im = (way & 1) != 0;
        const int side = 1 - (way & 2);
        const int32_t c = side * far_line->c;
        if (pass->floor.swell != 0 && pass->floor.far_swell != 0 && far_line->count != 0) {
            draw_far_swell_line(pass, far_line, along_im, side, lift, col);
            draw_far_ring(pass, &plane, along_im, c, BOOT_ANIM_FAR_SWELL_FADE_Q12, reach, col);
        } else {
            draw_far_ring(pass, &plane, along_im, c, 0, solid, col);
            draw_far_front(pass, &plane, along_im, c, solid, front, level);
        }
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
        .drawn = (int)((boot_anim_pen(now_ms) * (int32_t)(BOOT_ANIM_CURVE_POINTS - 1)) >> BOOT_ANIM_Q),
        .colour_pen = boot_anim_colour_progress(now_ms),
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

    draw_far_grid(&pass);
    draw_axis_lines_far(&pass);
}

/* The axes */

/* Arms arrive together despite differing lengths. */
static void
draw_arm(int32_t re, int32_t im, int32_t t, uint8_t reach, uint8_t ink, const boot_anim_view_t* view) {
    const int32_t fre = mathi_lerp_u8(0, re, reach);
    const int32_t fim = mathi_lerp_u8(0, im, reach);
    const int32_t ft = mathi_lerp_u8(0, t, reach);

    int ax;
    int ay;
    int bx;
    int by;
    if (boot_anim_project_segment(0, 0, 0, fre, fim, ft, view, &ax, &ay, &bx, &by)) {
        draw_line_ex(ax, ay, bx, by, lit(COL_AXIS, ink), 0U);
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
        int re_x;
        int re_y;
        int im_x;
        int im_y;
        int t_x;
        int t_y;
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
        int x;
        int y;
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

        draw_line_ex(ax, ay, bx, by, c, DRAW_LINE_ADD | (joined ? DRAW_LINE_OPEN : 0U));
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
        int x;
        int y;
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
    int nsx;
    int nsy;
    const bool next_front = r3d_project_point_cs_x(next_cs, view, &nsx, &nsy);

    if (segment->prev_front && next_front) {
        draw_stroke(segment->prev_sx, segment->prev_sy, nsx, nsy, color, width, segment->joined);
        segment->joined = true;
        segment->prev_sx = nsx;
        segment->prev_sy = nsy;
    } else if (segment->prev_front != next_front) {
        int ax;
        int ay;
        int bx;
        int by;
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
draw_curve(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view) {
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

        const boot_anim_stroke_t s = boot_anim_stroke(a0 + ((a1 - a0) >> 1), colour);
        gfx_color_t span_c = gfx_rgb(gfx_hue_rgb(s.hue));
        span_c = gfx_color_mix(span_c, COL_WHITE, s.bloom);
        span_c = gfx_color_mix(COL_BG, span_c, scale8(s.glow, ink));

        /* One chord a span, to where the spline ends it: the samples sit a
         * few pixels apart, so subdividing showed nothing on the panel. */
        draw_curve_segment(&segment, boot_anim_spline_cs(ta, tb, tc, limit), span_c, s.width, view);

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
        int px;
        int py;
        title_glyph_origin(p.x, p.y, glyph_w, glyph_h, &px, &py);

        one[0] = BOOT_ANIM_TITLE[i];
        if (has_shadow) {
            int shadow_dx;
            int shadow_dy;
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

/* Core 1 clears each frame once it is sent, so the clear above is free. */
void
boot_anim_clear_on_send(bool on) {
    (void)gfx_set_clear_on_send(on, COL_BG); /* without the double layout, the clear stays on core 0 */
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

        const int32_t reached = draw_curve(now_ms, ink, &view);
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
    const gfx_mode_request_t mode = {.layout = GFX_LAYOUT_DOUBLE_FB};
    gfx_mode_enter(&mode);
    boot_anim_clear_on_send(true);
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
    gfx_mode_exit();
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
