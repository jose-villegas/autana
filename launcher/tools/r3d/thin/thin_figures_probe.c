/* thin_figures_probe: four small pictures drawn by r3d_curve_draw() for the thin-curve figures, as JSON. */
#include <stdint.h>
#include <stdio.h>

#include "image/pixel/rgb565.h"
#include "render/curve/r3d_curve.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"

#define WIDTH       64
#define HEIGHT      40
#define PIXELS      (WIDTH * HEIGHT)
#define TICKS_PER_M 100 /* the lens maps one tick to TICKS_PER_M / depth pixels */
#define NEAR_Z      1.0F
#define BACKGROUND  RGB565BE(0x102030)
#define WALL        RGB565BE(0x707070)
#define PILLAR      RGB565BE(0x404850)
#define KNOTS       7
#define SAG_SHARE   0.35F /* the cable's sag, as a share of its half span */
#define WALL_DEPTH  1     /* the farthest depth a pixel can hold: the wall stands behind everything */
#define PILLAR_HALF 4     /* pixels either side of the picture's middle */

/* A sagging cable between two posts at one depth: half span and radius in ticks, colour from warm to cool. */
typedef struct {
    const char* title;
    int depth;
    int half_span;
    int radius;
    int scene; /* 0 empty, 1 a wall with a pillar in front */
} cable_t;

static const uint16_t knot_colors[KNOTS] = {
    RGB565BE(0xEB6834), RGB565BE(0xED8A2C), RGB565BE(0xEDA100), RGB565BE(0xB8B23A),
    RGB565BE(0x6FB15A), RGB565BE(0x1BAF7A), RGB565BE(0x2A9AC0),
};

static gfx_color_t color[PIXELS];
static uint16_t depth[PIXELS];

static r3d_lens_t
lens(void) {
    r3d_lens_t result = {.m = mat4f_identity(), .width = WIDTH, .height = HEIGHT, .depth_near = NEAR_Z};
    result.project = (projectf_t){1, -1, WIDTH / 2.0F, (HEIGHT / 2.0F) + 0.5F, NEAR_Z};
    result.m.m[0][0] = TICKS_PER_M;
    result.m.m[1][1] = TICKS_PER_M;
    return result;
}

static r3d_span_target_t
clear(int scene) {
    for (int i = 0; i < PIXELS; i++) {
        const int x = i % WIDTH;
        const int pillar = x >= (WIDTH / 2) - PILLAR_HALF && x < (WIDTH / 2) + PILLAR_HALF;
        color[i] = scene == 0 ? BACKGROUND : (pillar ? PILLAR : WALL);
        depth[i] = scene == 0 ? R3D_DEPTH_EMPTY : (pillar ? R3D_DEPTH_NEAREST : WALL_DEPTH);
    }
    r3d_span_target_t target = r3d_span_target(color, depth, WIDTH, 0, HEIGHT);
    target.clear = BACKGROUND;
    return target;
}

static void
draw(const r3d_curve_knot_t* knots, int count, const r3d_span_target_t* target) {
    const r3d_curve_t curve = {
        .count = (uint16_t)count, .lo = {INT16_MIN, INT16_MIN, INT16_MIN}, .hi = {INT16_MAX, INT16_MAX, INT16_MAX}};
    const r3d_curve_set_t set = {&curve, knots, 1};
    const r3d_lens_t camera = lens();
    r3d_curve_draw(set, &camera, target, false);
    r3d_curve_draw(set, &camera, target, true);
}

static void
print_picture(const char* title, int first) {
    printf("%s{\"title\":\"%s\",\"rgb\":[", first ? "" : ",", title);
    for (int i = 0; i < PIXELS; i++) {
        printf("%s%u", i ? "," : "", (unsigned)rgb565be_to_rgb888(color[i]));
    }
    printf("]}");
}

static void
cable(const cable_t* c, int first) {
    r3d_curve_knot_t knots[KNOTS];
    for (int k = 0; k < KNOTS; k++) {
        const float t = ((2.0F * (float)k) / (KNOTS - 1)) - 1.0F;
        knots[k] = (r3d_curve_knot_t){(int16_t)(t * (float)c->half_span),
                                      (int16_t)(SAG_SHARE * (float)c->half_span * ((t * t) - 0.5F)), (int16_t)c->depth,
                                      (uint16_t)c->radius, knot_colors[k]};
    }
    const r3d_span_target_t target = clear(c->scene);
    draw(knots, KNOTS, &target);
    print_picture(c->title, first);
}

/* A pole pointing straight at the camera: both knots land on one pixel, so the segment draws a square. */
static void
end_on(const char* title, int near, int far, int radius) {
    const r3d_curve_knot_t knots[] = {{0, 0, (int16_t)near, (uint16_t)radius, knot_colors[0]},
                                      {0, 0, (int16_t)far, (uint16_t)radius, knot_colors[0]}};
    const r3d_span_target_t target = clear(0);
    draw(knots, 2, &target);
    print_picture(title, 0);
}

int
main(void) {
    static const cable_t near = {"Near: a lit ribbon", 300, 84, 9, 0};
    static const cable_t far = {"Far: thinner than a pixel, faded", 3000, 840, 6, 0};
    static const cable_t walled = {"Over a wall, behind a pillar", 3000, 840, 6, 1};
    printf("{\"width\":%d,\"height\":%d,\"min_width_px\":%g,\"pictures\":[", WIDTH, HEIGHT,
           (double)R3D_CURVE_MIN_WIDTH_PX);
    cable(&near, 1);
    cable(&far, 0);
    end_on("End on: a square", 300, 900, 18);
    cable(&walled, 0);
    printf("]}\n");
    return 0;
}
