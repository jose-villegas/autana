/*
 * Portable suite: raster_draw() fits the depth encoding to the nearest thing
 * it kept, so two surfaces a hair apart far below the eye stay apart.
 *
 * The scene is the case that showed it: a ground square wide enough to
 * reach behind the eye, and a street just above it, seen from high up
 * looking down. Stored depth is depth_near / z in 16 bits, so two surfaces a
 * gap h apart seen from height H at view depth z are
 * R3D_DEPTH_NEAREST depth_near h / (H z) steps apart: under one step with
 * the clip plane's depth_near, and many with the fitted one.
 */

#include <stdint.h>
#include <stdlib.h>

#include "r3d_quad_mesh.h"
#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "render/r3d_span.h"
#include "render/raster.h"

#define SIZE         64
#define CLEAR        0x0000
#define GROUND_COLOR 0x07E0
#define STREET_COLOR 0xF800
#define GROUND_REACH 30000 /* the ground square's half side: far behind the eye */
#define STREET_REACH 5000  /* the street's half side: past the middle half of the picture */
#define STREET_GAP   2     /* the street's height over the ground */
#define EYE_HEIGHT   2600
#define NEAR_Z       6.0F
#define HALF_FOV_TAN 0.62F

/* Looking down at 45 degrees onto the origin. */
static const fixture_camera_t CAMERA = {
    {0.0F, (float)EYE_HEIGHT, -(float)EYE_HEIGHT}, {0.0F, -1.0F, 1.0F}, HALF_FOV_TAN, NEAR_Z};

typedef struct {
    int16_t corners[4][3];
    uint16_t faces[2];
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
    r3d_lit_mesh_t mesh;
} slab_t;

/* A square of half side `reach` at height `y`, one colour, facing up. */
static void
make_slab(slab_t* s, int16_t reach, int16_t y, uint16_t color) {
    const int16_t xs[4] = {(int16_t)-reach, reach, reach, (int16_t)-reach};
    const int16_t zs[4] = {(int16_t)-reach, (int16_t)-reach, reach, reach};
    box3s_t bounds = box3s_empty();
    for (int i = 0; i < 4; i++) {
        s->corners[i][0] = xs[i];
        s->corners[i][1] = y;
        s->corners[i][2] = zs[i];
        bounds = box3s_extend(bounds, vec3s_from_array(s->corners[i]));
    }
    s->faces[0] = s->faces[1] = color;
    s->cluster = (r3d_lit_cluster_t){0, 4, 0, 2, bounds, true};
    s->node = (r3d_lit_node_t){bounds, 0, 1, true};
    s->mesh = r3d_quad_mesh(s->corners, NULL, s->faces, &s->cluster, &s->node);
}

typedef struct {
    slab_t ground, street;
    r3d_instance_t instances[2];
    raster_t raster;
} street_scene_t;

/* The ground draws first, so the street shows only where it is stored
 * strictly nearer: a tie keeps the ground. */
static street_scene_t*
draw_street(void) {
    street_scene_t* s = malloc(sizeof *s);
    TEST_ASSERT_NOT_NULL(s);
    make_slab(&s->ground, GROUND_REACH, 0, GROUND_COLOR);
    make_slab(&s->street, STREET_REACH, STREET_GAP, STREET_COLOR);
    s->instances[0] = (r3d_instance_t){&s->ground.mesh, NULL};
    s->instances[1] = (r3d_instance_t){&s->street.mesh, NULL};
    s->raster =
        (raster_t){.instances = s->instances, .instance_count = 2, .width = SIZE, .height = SIZE, .clear = CLEAR};
    s->raster.scratch = malloc(raster_scratch_bytes(&s->raster));
    TEST_ASSERT_NOT_NULL(s->raster.scratch);
    const render_view_t view = render_view_fixture(&CAMERA, &s->raster, 0);
    (void)raster_draw(&s->raster, &view);
    return s;
}

static void
free_street(street_scene_t* s) {
    free(s->raster.scratch);
    free(s);
}

static void
test_a_street_just_over_the_ground_shows_whole_from_high_up(void) {
    street_scene_t* s = draw_street();
    const uint16_t* color = raster_color(&s->raster);
    /* The street covers the middle of the picture: the point looked at
     * and well past a pixel around it. */
    const int quarter = SIZE / 4;
    int ground_shows = 0;
    for (int y = quarter; y < SIZE - quarter; y++) {
        for (int x = quarter; x < SIZE - quarter; x++) {
            ground_shows += color[(y * SIZE) + x] != STREET_COLOR;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, ground_shows);
    free_street(s);
}

static void
test_the_fit_reaches_past_the_clip_plane_and_saturates_nothing(void) {
    street_scene_t* s = draw_street();
    const float depth_near = raster_depth_near(&s->raster);
    TEST_ASSERT_TRUE(depth_near > NEAR_Z);
    /* Nothing drawn is nearer than the fit: no stored depth reaches its ceiling. */
    const uint16_t* depth = raster_depth(&s->raster);
    for (int i = 0; i < SIZE * SIZE; i++) {
        TEST_ASSERT_TRUE(depth[i] < R3D_DEPTH_NEAREST);
    }
    /* The bottom row's ray meets the ground nearest; the fit is at most its depth. */
    const float drawn_nearest = depth_near * (float)R3D_DEPTH_NEAREST / (float)depth[(SIZE - 1) * SIZE + (SIZE / 2)];
    TEST_ASSERT_TRUE(depth_near <= drawn_nearest);
    free_street(s);
}

void
suite_raster_depth_fit(void) {
    RUN_TEST(test_a_street_just_over_the_ground_shows_whole_from_high_up);
    RUN_TEST(test_the_fit_reaches_past_the_clip_plane_and_saturates_nothing);
}

SUITE_REGISTER(suite_raster_depth_fit);
