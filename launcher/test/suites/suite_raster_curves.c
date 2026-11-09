/*
 * Portable suite: the curves of a lit mesh drawn through raster_draw(), as a frame draws them: a wall behind a thin
 * line and a card before it show what batches, depth, models, the depth fit and the upscale do to a curve.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565.h"
#include "r3d_quad_mesh.h"
#include "render/curve/r3d_curve.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/raster_internal.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#define SIZE           64
#define PIXELS         (SIZE * SIZE)

#define CLEAR          RGB565BE(0x102030)
#define WALL           RGB565BE(0x008000)
#define CARD           RGB565BE(0x0000FF)
#define RED            RGB565BE(0xFF0000)
#define FADED          RGB565BE(0xFFFF00)

#define NEAR_Z         1.0F
#define HALF_FOV_TAN   0.5F
#define CAMERA_BACK    300 /* the eye stands this far in front of the wall's plane, z = 0 */

#define WALL_HALF      400
#define CARD_HALF      80
#define CARD_Z         (-100)
#define LINE_Z         (-50)
#define LINE_HALF      60
#define THICK          10 /* a ribbon this wide at the line's depth is several pixels: drawn opaque */
#define THIN           1  /* and this one is under a pixel: faded */
#define WALL_AWAY      (10 * WALL_HALF)
#define STUB_SIDE      2000 /* a mesh's own quad this far off to the side is outside every view here */
#define STUB_HALF      8

#define NEAR_LINE_Z    (-250) /* 50 from the eye, nearer than the wall or the card */
#define NEAR_LINE_HALF 20
#define NEAR_THICK     5
#define BEHIND_Z       200 /* behind the wall */
#define NEAR_FADED_Z   (-150)

/* The eye's distance to a plane at z. */
#define EYE_TO(z)      (CAMERA_BACK + (z))

static const fixture_camera_t CAMERA = {{0.0F, 0.0F, -(float)CAMERA_BACK}, {0.0F, 0.0F, 1.0F}, HALF_FOV_TAN, NEAR_Z};

typedef struct {
    int16_t corners[4][3];
    uint16_t faces[2];
    r3d_quad_t quad;
    r3d_curve_t* curves; /* an LCRV entry's rows and, right after them, its knots; or NULL */
} part_t;

/* A two-knot curve: a line from (x0, y0) to (x1, y1) in the plane z. */
typedef struct {
    int16_t x0, y0, x1, y1, z;
    uint16_t radius, color;
} line_t;

#define PARTS 5

typedef struct {
    part_t wall, card;
    part_t curve[PARTS];
    mat4f_t model[2];
    r3d_instance_t instance[3];
    raster_t raster;
    render_view_t view;
    uint16_t* destination;
} scene_t;

static scene_t* scene;

static void
release(void) {
    if (scene != NULL) {
        for (int i = 0; i < PARTS; i++) {
            free(scene->curve[i].curves);
        }
        free(scene->raster.scratch);
        free(scene->destination);
        free(scene);
        scene = NULL;
    }
}

/* A square of half side `half` centred on (cx, 0) in the plane z, one colour. */
static void
part_square(part_t* p, int16_t cx, int16_t half, int16_t z, uint16_t color) {
    const int16_t xs[4] = {(int16_t)(cx - half), (int16_t)(cx + half), (int16_t)(cx + half), (int16_t)(cx - half)};
    const int16_t ys[4] = {(int16_t)-half, (int16_t)-half, half, half};
    for (int i = 0; i < 4; i++) {
        p->corners[i][0] = xs[i];
        p->corners[i][1] = ys[i];
        p->corners[i][2] = z;
    }
    p->faces[0] = p->faces[1] = color;
    r3d_quad_init(&p->quad, p->corners);
    p->quad.mesh.colors = NULL;
    p->quad.mesh.face_colors = p->faces;
}

static int16_t
least(int16_t a, int16_t b) {
    return a < b ? a : b;
}

static int16_t
most(int16_t a, int16_t b) {
    return a > b ? a : b;
}

/* Gives the part its `count` lines as curves, each box just holding its tube. */
static void
part_lines(part_t* p, const line_t* lines, int count) {
    free(p->curves);
    r3d_curve_t* block = malloc((size_t)count * (sizeof(r3d_curve_t) + (2 * sizeof(r3d_curve_knot_t))));
    TEST_ASSERT_NOT_NULL(block);
    r3d_curve_knot_t* knots = (r3d_curve_knot_t*)(void*)(block + count);
    for (int i = 0; i < count; i++) {
        const line_t* l = &lines[i];
        knots[2 * i] = (r3d_curve_knot_t){l->x0, l->y0, l->z, l->radius, l->color};
        knots[(2 * i) + 1] = (r3d_curve_knot_t){l->x1, l->y1, l->z, l->radius, l->color};
        const int16_t r = (int16_t)l->radius;
        block[i] =
            (r3d_curve_t){(uint16_t)(2 * i),
                          2,
                          {(int16_t)(least(l->x0, l->x1) - r), (int16_t)(least(l->y0, l->y1) - r), (int16_t)(l->z - r)},
                          {(int16_t)(most(l->x0, l->x1) + r), (int16_t)(most(l->y0, l->y1) + r), (int16_t)(l->z + r)}};
    }
    p->curves = block;
    p->quad.mesh.curves = block;
    p->quad.mesh.curve_count = (uint16_t)count;
}

/* A wall behind everything, a card in front of the lines, and PARTS meshes that carry curves on a quad of their
 * own out of every view: it draws nothing, so what shows is the curves. */
static scene_t*
scene_open(void) {
    scene = calloc(1, sizeof(*scene));
    TEST_ASSERT_NOT_NULL(scene);
    suite_set_test_cleanup(release);
    part_square(&scene->wall, 0, WALL_HALF, 0, WALL);
    part_square(&scene->card, 0, CARD_HALF, CARD_Z, CARD);
    for (int i = 0; i < PARTS; i++) {
        part_square(&scene->curve[i], STUB_SIDE, STUB_HALF, 0, WALL);
    }
    for (int i = 0; i < 2; i++) {
        scene->model[i] = mat4f_identity();
    }
    return scene;
}

static void
place(scene_t* s, int slot, const part_t* part, const mat4f_t* model) {
    s->instance[slot] = (r3d_instance_t){&part->quad.mesh, model, NULL, NULL};
}

/* The raster over the first `count` instances; `upscaled` for a same-size upscale into `destination`. */
static void
scene_raster(scene_t* s, int count, bool upscaled) {
    free(s->raster.scratch);
    s->raster = (raster_t){.instances = s->instance,
                           .instance_count = count,
                           .width = SIZE,
                           .height = SIZE,
                           .clear = CLEAR,
                           .upscaled = upscaled,
                           .destination_width = upscaled ? SIZE : 0,
                           .destination_height = upscaled ? SIZE : 0};
    s->raster.scratch = malloc(raster_scratch_bytes(&s->raster));
    TEST_ASSERT_NOT_NULL(s->raster.scratch);
}

static void
draw(scene_t* s) {
    s->view = render_view_fixture(&CAMERA, &s->raster, 0);
    (void)raster_draw(&s->raster, &s->view);
}

static int
count_of(const gfx_color_t* picture, gfx_color_t color) {
    int count = 0;
    for (int i = 0; i < PIXELS; i++) {
        count += picture[i] == color;
    }
    return count;
}

/* Every pixel is one of `allowed`'s two colours. */
static void
expect_only(const gfx_color_t* picture, gfx_color_t a, gfx_color_t b) {
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(picture[i] == a || picture[i] == b, "a pixel of neither colour");
    }
}

/* The share of a ribbon's colour that mixes in over what is behind it: its true width over the least one drawn. */
static uint8_t
faded_share(const scene_t* s, int radius, int distance) {
    r3d_lens_t lens;
    raster_lens(&s->raster, &s->view, 1, &lens);
    const float coverage = 2.0F * (float)radius * lens.focal_x / (float)distance / R3D_CURVE_MIN_WIDTH_PX;
    TEST_ASSERT_TRUE(coverage > 0.0F && coverage < 1.0F);
    return (uint8_t)lroundf(coverage * R3D_SPAN_COVERAGE_FULL);
}

static void
test_a_faded_curve_mixes_once_over_a_wall_however_many_batches_draw(void) {
    scene_t* s = scene_open();
    const line_t line = {-LINE_HALF, 0, LINE_HALF, 0, LINE_Z, THIN, FADED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 0, &s->wall, NULL);
    place(s, 1, &s->curve[0], NULL);
    place(s, 2, &s->curve[1], NULL);
    scene_raster(s, 3, false);
    TEST_ASSERT_TRUE_MESSAGE(raster_batch_end(&s->raster, 0) < 3, "the instances draw in one batch");
    draw(s);
    const gfx_color_t mixed = rgb565be_mix(WALL, FADED, faded_share(s, THIN, EYE_TO(LINE_Z)));
    TEST_ASSERT_TRUE(mixed != WALL && mixed != FADED);
    expect_only(raster_color(&s->raster), WALL, mixed);
    TEST_ASSERT_GREATER_THAN_INT(0, count_of(raster_color(&s->raster), mixed));
}

static void
test_a_curve_behind_a_nearer_cluster_is_hidden(void) {
    scene_t* s = scene_open();
    const line_t line = {-LINE_HALF, 0, LINE_HALF, 0, LINE_Z, THICK, RED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 0, &s->wall, NULL);
    place(s, 1, &s->curve[0], NULL);
    place(s, 2, &s->card, NULL);
    scene_raster(s, 3, false);
    draw(s);
    expect_only(raster_color(&s->raster), WALL, CARD);
    TEST_ASSERT_GREATER_THAN_INT(0, count_of(raster_color(&s->raster), CARD));
}

static void
test_a_curve_in_front_of_a_farther_cluster_shows(void) {
    scene_t* s = scene_open();
    const line_t line = {-LINE_HALF, 0, LINE_HALF, 0, LINE_Z, THICK, RED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 0, &s->wall, NULL);
    place(s, 1, &s->curve[0], NULL);
    scene_raster(s, 2, false);
    draw(s);
    expect_only(raster_color(&s->raster), WALL, RED);
    TEST_ASSERT_GREATER_THAN_INT(0, count_of(raster_color(&s->raster), RED));
}

typedef struct {
    int left, right, top, bottom;
} extent_t;

/* The columns and rows `color` fills, or an inverted extent when it fills none. */
static extent_t
extent_of(const gfx_color_t* picture, gfx_color_t color) {
    extent_t e = {SIZE, -1, SIZE, -1};
    for (int i = 0; i < PIXELS; i++) {
        if (picture[i] == color) {
            e.left = i % SIZE < e.left ? i % SIZE : e.left;
            e.right = i % SIZE > e.right ? i % SIZE : e.right;
            e.top = i / SIZE < e.top ? i / SIZE : e.top;
            e.bottom = i / SIZE > e.bottom ? i / SIZE : e.bottom;
        }
    }
    return e;
}

static void
expect_extents_within(int tolerance, extent_t want, extent_t got) {
    TEST_ASSERT_INT_WITHIN(tolerance, want.left, got.left);
    TEST_ASSERT_INT_WITHIN(tolerance, want.right, got.right);
    TEST_ASSERT_INT_WITHIN(tolerance, want.top, got.top);
    TEST_ASSERT_INT_WITHIN(tolerance, want.bottom, got.bottom);
}

/* A model that doubles and moves an instance puts its curves where the same curves, written already doubled and
 * moved, stand without one, and as wide. */
static void
test_a_model_places_and_sizes_an_instance_s_curves_like_its_transformed_geometry(void) {
    enum { SCALE = 2, MOVE_X = 30, MOVE_Y = -20, RADIUS = 5, HALF = 30, TILT = 10 };

    scene_t* s = scene_open();
    const line_t own = {-HALF, -TILT, HALF, TILT, LINE_Z / SCALE, RADIUS, RED};
    const line_t baked = {(int16_t)((SCALE * own.x0) + MOVE_X),
                          (int16_t)((SCALE * own.y0) + MOVE_Y),
                          (int16_t)((SCALE * own.x1) + MOVE_X),
                          (int16_t)((SCALE * own.y1) + MOVE_Y),
                          (int16_t)(SCALE * own.z),
                          (uint16_t)(SCALE * own.radius),
                          RED};
    part_lines(&s->curve[0], &own, 1);
    part_lines(&s->curve[1], &baked, 1);
    mat4f_t* model = &s->model[0];
    model->m[0][0] = model->m[1][1] = model->m[2][2] = SCALE;
    model->m[0][3] = MOVE_X;
    model->m[1][3] = MOVE_Y;
    place(s, 0, &s->wall, NULL);
    place(s, 1, &s->curve[1], NULL);
    scene_raster(s, 2, false);
    draw(s);
    const extent_t want = extent_of(raster_color(&s->raster), RED);
    TEST_ASSERT_GREATER_THAN_INT(0, want.right - want.left);

    place(s, 1, &s->curve[0], model);
    draw(s);
    expect_extents_within(1, want, extent_of(raster_color(&s->raster), RED));

    place(s, 1, &s->curve[0], NULL);
    draw(s);
    const extent_t unplaced = extent_of(raster_color(&s->raster), RED);
    TEST_ASSERT_GREATER_THAN_INT(1, abs(unplaced.right - want.right));
}

/* The raster draws a picture's rows in two slices, one per core; a draw of the curves alone over the whole
 * picture is the picture they make together, the lens from the view the way the raster composes it. */
static void
test_the_two_slice_draw_equals_a_whole_picture_draw(void) {
    scene_t* s = scene_open();
    const line_t lines[] = {{-LINE_HALF, -LINE_HALF, LINE_HALF, LINE_HALF, LINE_Z, THICK, RED},
                            {-LINE_HALF, LINE_HALF, LINE_HALF, -LINE_HALF, LINE_Z, THIN, FADED},
                            {0, -LINE_HALF, 0, LINE_HALF, LINE_Z, THIN, CARD}};
    part_lines(&s->curve[0], lines, 3);
    place(s, 0, &s->curve[0], NULL);
    scene_raster(s, 1, false);
    draw(s);

    gfx_color_t* color = malloc(PIXELS * sizeof(*color));
    uint16_t* depth = malloc(PIXELS * sizeof(*depth));
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    for (int i = 0; i < PIXELS; i++) {
        color[i] = CLEAR;
        depth[i] = R3D_DEPTH_EMPTY;
    }
    r3d_span_target_t target = r3d_span_target(color, depth, SIZE, 0, SIZE);
    target.clear = CLEAR;
    r3d_lens_t lens;
    raster_lens(&s->raster, &s->view, s->curve[0].quad.mesh.position_scale, &lens);
    r3d_lens_set_depth_near(&lens, raster_depth_near(&s->raster));
    const r3d_curve_set_t set = r3d_lit_mesh_curves(&s->curve[0].quad.mesh);
    r3d_curve_draw(set, &lens, &target, false);
    r3d_curve_draw(set, &lens, &target, true);

    TEST_ASSERT_GREATER_THAN_INT(0, count_of(color, RED));
    TEST_ASSERT_GREATER_THAN_INT(0, count_of(color, rgb565be_mix(CLEAR, FADED, faded_share(s, THIN, EYE_TO(LINE_Z)))));
    TEST_ASSERT_EQUAL_HEX16_ARRAY(color, raster_color(&s->raster), PIXELS);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(depth, raster_depth(&s->raster), PIXELS);
    free(color);
    free(depth);
}

/* With an upscale the raster leaves the colour of a picture as the last frame had it and the upscale supplies the
 * clear colour where depth is empty. A faded ribbon over nothing mixes with the clear colour, not that old pixel,
 * and takes its depth so the upscale shows it. */
static void
test_a_faded_curve_over_empty_depth_shows_through_the_upscale_and_keeps_no_old_pixel(void) {
    scene_t* s = scene_open();
    const line_t line = {-LINE_HALF, 0, LINE_HALF, 0, LINE_Z, THIN, FADED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 0, &s->wall, &s->model[0]);
    place(s, 1, &s->curve[0], NULL);
    scene_raster(s, 2, true);
    s->destination = malloc(PIXELS * sizeof(*s->destination));
    TEST_ASSERT_NOT_NULL(s->destination);

    draw(s);
    s->model[0].m[0][3] = WALL_AWAY; /* the wall leaves the view: this frame holds the curve and nothing else */
    draw(s);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(WALL, raster_color(&s->raster)[0], "the last frame's pixels are not left in place");
    raster_upscale(&s->raster, s->destination, SIZE, SIZE);

    const gfx_color_t mixed = rgb565be_mix(CLEAR, FADED, faded_share(s, THIN, EYE_TO(LINE_Z)));
    TEST_ASSERT_TRUE(mixed != CLEAR && mixed != WALL);
    int shown = 0;
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_TRUE_MESSAGE(s->destination[i] == CLEAR || s->destination[i] == mixed, "a pixel of neither colour");
        shown += s->destination[i] == mixed;
    }
    TEST_ASSERT_GREATER_THAN_INT(0, shown);
}

/* A thin, near ribbon mixes over a thick, far one that is declared after it as over one declared before it. */
static const line_t FAR_OPAQUE = {-LINE_HALF, 0, LINE_HALF, 0, 0, THICK, RED};
static const line_t NEAR_FADED = {-LINE_HALF / 2, 0, LINE_HALF / 2, 0, NEAR_FADED_Z, THIN, FADED};

static void
expect_ribbon_over_ribbon(const scene_t* s, const gfx_color_t* picture) {
    TEST_ASSERT_GREATER_THAN_INT(
        0, count_of(picture, rgb565be_mix(RED, FADED, faded_share(s, THIN, EYE_TO(NEAR_FADED_Z)))));
}

static void
test_opaque_and_faded_curves_of_one_mesh_draw_the_same_in_either_declaration_order(void) {
    scene_t* s = scene_open();
    const line_t opaque_first[] = {FAR_OPAQUE, NEAR_FADED};
    const line_t faded_first[] = {NEAR_FADED, FAR_OPAQUE};
    part_lines(&s->curve[0], opaque_first, 2);
    part_lines(&s->curve[1], faded_first, 2);
    place(s, 0, &s->curve[0], NULL);
    scene_raster(s, 1, false);
    draw(s);
    gfx_color_t* before = malloc(PIXELS * sizeof(*before));
    TEST_ASSERT_NOT_NULL(before);
    memcpy(before, raster_color(&s->raster), PIXELS * sizeof(*before));
    expect_ribbon_over_ribbon(s, before);

    place(s, 0, &s->curve[1], NULL);
    draw(s);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(before, raster_color(&s->raster), PIXELS);
    free(before);
}

static void
test_opaque_and_faded_curves_of_two_instances_draw_the_same_in_either_order(void) {
    scene_t* s = scene_open();
    part_lines(&s->curve[0], &FAR_OPAQUE, 1);
    part_lines(&s->curve[1], &NEAR_FADED, 1);
    place(s, 0, &s->curve[0], NULL);
    place(s, 1, &s->curve[1], NULL);
    scene_raster(s, 2, false);
    draw(s);
    gfx_color_t* before = malloc(PIXELS * sizeof(*before));
    TEST_ASSERT_NOT_NULL(before);
    memcpy(before, raster_color(&s->raster), PIXELS * sizeof(*before));
    expect_ribbon_over_ribbon(s, before);

    place(s, 0, &s->curve[1], NULL);
    place(s, 1, &s->curve[0], NULL);
    draw(s);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(before, raster_color(&s->raster), PIXELS);
    free(before);
}

/* The depth scale the picture was drawn with after a wall alone. */
static float
wall_alone_depth_near(scene_t* s) {
    place(s, 0, &s->wall, NULL);
    scene_raster(s, 1, false);
    draw(s);
    return raster_depth_near(&s->raster);
}

static void
test_a_curve_behind_the_wall_leaves_the_depth_fit_where_the_clusters_put_it(void) {
    scene_t* s = scene_open();
    const float alone = wall_alone_depth_near(s);
    TEST_ASSERT_TRUE(alone > NEAR_Z);
    const line_t line = {-LINE_HALF, 0, LINE_HALF, 0, BEHIND_Z, THICK, RED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 1, &s->curve[0], NULL);
    scene_raster(s, 2, false);
    draw(s);
    TEST_ASSERT_EQUAL_FLOAT(alone, raster_depth_near(&s->raster));
}

static void
test_a_curve_nearer_than_every_cluster_lowers_the_depth_fit(void) {
    scene_t* s = scene_open();
    const float alone = wall_alone_depth_near(s);
    const line_t line = {-NEAR_LINE_HALF, 0, NEAR_LINE_HALF, 0, NEAR_LINE_Z, NEAR_THICK, RED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 1, &s->curve[0], NULL);
    scene_raster(s, 2, false);
    draw(s);
    const float fitted = raster_depth_near(&s->raster);
    TEST_ASSERT_TRUE(fitted < alone);
    TEST_ASSERT_TRUE(fitted >= NEAR_Z);
    /* Drawn at the fitted scale it is in front of the wall, whole, and short of the depth ceiling. */
    TEST_ASSERT_GREATER_THAN_INT(0, count_of(raster_color(&s->raster), RED));
    expect_only(raster_color(&s->raster), WALL, RED);
    for (int i = 0; i < PIXELS; i++) {
        TEST_ASSERT_TRUE(raster_depth(&s->raster)[i] <= R3D_DEPTH_NEAREST);
    }
}

#define ACROSS_EYE_DIST 64 /* a power of two: the depth scale steps by them */
#define ACROSS_Z        (ACROSS_EYE_DIST - CAMERA_BACK)

/* The half width of the view at the plane z: the eye's distance to it times the lens's tangent. */
#define VIEW_HALF_AT(z) ((int)(EYE_TO(z) * HALF_FOV_TAN))

/* The depth scale of a wall and, over it, the near line's ribbon centred on x = cx in the plane z. */
static float
depth_fit_with_near_line_at(scene_t* s, int cx, int z) {
    const line_t line = {
        (int16_t)(cx - NEAR_LINE_HALF), 0, (int16_t)(cx + NEAR_LINE_HALF), 0, (int16_t)z, NEAR_THICK, RED};
    part_lines(&s->curve[0], &line, 1);
    place(s, 0, &s->wall, NULL);
    place(s, 1, &s->curve[0], NULL);
    scene_raster(s, 2, false);
    draw(s);
    return raster_depth_near(&s->raster);
}

static void
test_a_curve_box_outside_the_view_leaves_the_depth_fit_to_the_clusters(void) {
    scene_t* s = scene_open();
    const float alone = wall_alone_depth_near(s);
    /* The whole box, near face and far face alike, is past the side plane. */
    const int aside = (2 * VIEW_HALF_AT(NEAR_LINE_Z)) + NEAR_LINE_HALF + NEAR_THICK;
    TEST_ASSERT_EQUAL_FLOAT(alone, depth_fit_with_near_line_at(s, aside, NEAR_LINE_Z));
    TEST_ASSERT_TRUE(depth_fit_with_near_line_at(s, 0, NEAR_LINE_Z) < alone);
}

static void
test_a_curve_box_across_a_side_plane_lowers_the_fit_only_as_far_as_its_visible_part(void) {
    scene_t* s = scene_open();
    const float alone = wall_alone_depth_near(s);
    const float whole = depth_fit_with_near_line_at(s, 0, ACROSS_Z);
    /* The box's side face lies one past the side plane's reach at the line's own depth: its near edge is outside the
     * view, so the nearest part in view is where the plane meets the box's far face, past the next scale step. */
    const int across = VIEW_HALF_AT(ACROSS_Z) + 1 + NEAR_LINE_HALF + NEAR_THICK;
    const float cut = depth_fit_with_near_line_at(s, across, ACROSS_Z);
    TEST_ASSERT_TRUE(cut < alone);
    TEST_ASSERT_TRUE_MESSAGE(cut > whole, "what is outside the view does not count");
}

static void
test_with_the_depth_fit_off_a_nearer_curve_leaves_the_unfitted_scale(void) {
    scene_t* s = scene_open();
    const float fitted = depth_fit_with_near_line_at(s, 0, NEAR_LINE_Z);
    TEST_ASSERT_TRUE(fitted > NEAR_Z);
    render_settings_t off = RENDER_SETTINGS_DEFAULT;
    off.depth_fit = false;
    s->raster.settings = &off;
    draw(s);
    TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, raster_depth_near(&s->raster));
}

void
suite_raster_curves(void) {
    RUN_TEST(test_a_faded_curve_mixes_once_over_a_wall_however_many_batches_draw);
    RUN_TEST(test_a_curve_behind_a_nearer_cluster_is_hidden);
    RUN_TEST(test_a_curve_in_front_of_a_farther_cluster_shows);
    RUN_TEST(test_a_model_places_and_sizes_an_instance_s_curves_like_its_transformed_geometry);
    RUN_TEST(test_the_two_slice_draw_equals_a_whole_picture_draw);
    RUN_TEST(test_a_faded_curve_over_empty_depth_shows_through_the_upscale_and_keeps_no_old_pixel);
    RUN_TEST(test_opaque_and_faded_curves_of_one_mesh_draw_the_same_in_either_declaration_order);
    RUN_TEST(test_opaque_and_faded_curves_of_two_instances_draw_the_same_in_either_order);
    RUN_TEST(test_a_curve_behind_the_wall_leaves_the_depth_fit_where_the_clusters_put_it);
    RUN_TEST(test_a_curve_nearer_than_every_cluster_lowers_the_depth_fit);
    RUN_TEST(test_a_curve_box_outside_the_view_leaves_the_depth_fit_to_the_clusters);
    RUN_TEST(test_a_curve_box_across_a_side_plane_lowers_the_fit_only_as_far_as_its_visible_part);
    RUN_TEST(test_with_the_depth_fit_off_a_nearer_curve_leaves_the_unfitted_scale);
}

SUITE_REGISTER(suite_raster_curves);
