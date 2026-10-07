/*
 * Portable suite: raster_motion.h's attachment against motion worked out
 * apart from it. Each expected vector comes from a ray cast through the
 * pixel at the analytic quads the meshes are, the point it hits carried to
 * where it was and projected through the previous camera's own basis: no
 * depth buffer and none of the attachment's matrices.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "raster_rig.h"
#include "suites.h"
#include "unity.h"

#include "render/r3d.h"
#include "render/r3d_pipeline.h"
#include "render/raster_motion.h"
#include "render/ray.h"

#define W     96
#define H     64
#define WALL  0
#define BOX   1
#define NONE  (-1)
/* Half-pixel storage rounds by a quarter pixel; 16-bit inverse depth over
 * these distances adds a few hundredths. */
#define SLACK 0.35F

/* The rig's wall, then a box face 80 square about its own origin, also
 * facing +z, placed wherever a test stands it. */
static const int16_t box_positions[][3] = {{-40, -40, 0}, {40, -40, 0}, {40, 40, 0}, {-40, 40, 0}};
static const int16_t (*const wall_and_box[])[3] = {raster_rig_wall, box_positions};

/* The box turned `degrees` about y and standing at `at`. */
static r3d_placement_t
turned(float degrees, vec3f_t at) {
    const float a = degrees * 3.14159265F / 180.0F;
    const float c = cosf(a);
    const float s = sinf(a);
    return (r3d_placement_t){{{c, 0.0F, s}, {0.0F, 1.0F, 0.0F}, {-s, 0.0F, c}}, at};
}

static vec3f_t
row_of(const r3d_placement_t* p, int r) {
    return (vec3f_t){p->m[r][0], p->m[r][1], p->m[r][2]};
}

static vec3f_t
column_of(const r3d_placement_t* p, int c) {
    return (vec3f_t){p->m[0][c], p->m[1][c], p->m[2][c]};
}

static vec3f_t
place(const r3d_placement_t* p, vec3f_t v) {
    const vec3f_t turned_v = {vec3f_dot(row_of(p, 0), v), vec3f_dot(row_of(p, 1), v), vec3f_dot(row_of(p, 2), v)};
    return vec3f_add(turned_v, p->position);
}

/* The same with the transpose of m, which undoes a rotation. */
static vec3f_t
unplace(const r3d_placement_t* p, vec3f_t w) {
    const vec3f_t d = vec3f_sub(w, p->position);
    return (vec3f_t){vec3f_dot(column_of(p, 0), d), vec3f_dot(column_of(p, 1), d), vec3f_dot(column_of(p, 2), d)};
}

typedef struct {
    vec3f_t right, down, forward;
} basis_t;

/* The picture's axes as a camera_t sets them: right of forward on the
 * level, down under both. */
static basis_t
basis(const camera_t* c) {
    const vec3f_t f = vec3f_normalize(c->forward);
    const vec3f_t right = vec3f_normalize(vec3f_cross(f, (vec3f_t){0.0F, 1.0F, 0.0F}));
    return (basis_t){right, vec3f_cross(f, right), f};
}

/* Where `world` lands in a w x h picture of camera `c`, false behind it. */
static bool
project(const camera_t* c, int w, int h, vec3f_t world, float* x, float* y) {
    const basis_t b = basis(c);
    const vec3f_t d = vec3f_sub(world, c->eye);
    const float z = vec3f_dot(d, b.forward);
    if (z <= 0.0F) {
        return false;
    }
    const float k = (float)(w < h ? w : h) / (2.0F * c->half_fov_short_tan);
    *x = ((float)w * 0.5F) + (k * vec3f_dot(d, b.right) / z);
    *y = ((float)h * 0.5F) + (k * vec3f_dot(d, b.down) / z);
    return true;
}

/* A ray against the quad `half` units each side of the origin on z = 0,
 * carried by `p`: the distance along it, or a negative one for a miss. */
static float
hit_quad(const r3d_placement_t* p, float half, vec3f_t origin, vec3f_t dir) {
    const vec3f_t o = unplace(p, origin);
    const vec3f_t d = unplace(&(r3d_placement_t){{{p->m[0][0], p->m[0][1], p->m[0][2]},
                                                  {p->m[1][0], p->m[1][1], p->m[1][2]},
                                                  {p->m[2][0], p->m[2][1], p->m[2][2]}},
                                                 {0.0F, 0.0F, 0.0F}},
                              dir);
    if (fabsf(d.z) < 1e-6F) {
        return -1.0F;
    }
    const float t = -o.z / d.z;
    const float x = o.x + (t * d.x);
    const float y = o.y + (t * d.y);
    return t > 0.0F && fabsf(x) <= half && fabsf(y) <= half ? t : -1.0F;
}

typedef struct {
    camera_t camera;
    const r3d_placement_t* box; /* NULL: no box */
} pose_t;

/* What pixel (px, py) of a w x h picture of `now` shows, and where that
 * point was in a picture of `before` at the same size. */
static int
truth(const pose_t* now, const pose_t* before, int w, int h, int px, int py, float* mx, float* my) {
    const basis_t b = basis(&now->camera);
    ray_camera_t ray;
    ray_camera_init(&ray, now->camera.eye, b.forward, b.right, vec3f_scale(b.down, -1.0F),
                    now->camera.half_fov_short_tan, (viewport_t){w, h, 0});
    const vec3f_t dir = ray_direction(&ray, px, py);
    const r3d_placement_t still = turned(0.0F, (vec3f_t){0.0F, 0.0F, 0.0F});
    const float t_wall = hit_quad(&still, 400.0F, now->camera.eye, dir);
    const float t_box = now->box == NULL ? -1.0F : hit_quad(now->box, 40.0F, now->camera.eye, dir);
    const int what = t_box > 0.0F && (t_wall < 0.0F || t_box < t_wall) ? BOX : t_wall > 0.0F ? WALL : NONE;
    if (what == NONE) {
        return NONE;
    }
    const vec3f_t hit = vec3f_add(now->camera.eye, vec3f_scale(dir, what == BOX ? t_box : t_wall));
    const vec3f_t was = what == BOX ? place(before->box, unplace(now->box, hit)) : hit;
    float x;
    float y;
    if (!project(&before->camera, w, h, was, &x, &y)) {
        return NONE;
    }
    *mx = x - ((float)px + 0.5F);
    *my = y - ((float)py + 0.5F);
    return what;
}

/* What motion's attachment points at, in the rig's own room. */
typedef struct {
    raster_motion_t motion;
    raster_attachment_t attachment;
    const raster_attachment_t* attached[1];
} motion_own_t;

static motion_own_t*
own_of(raster_rig_t* r) {
    return (motion_own_t*)r->own;
}

/* Allocated per test: the wall and, when `with_box`, the box placed by
 * placement[1], motion attached unless `detached`, pictures up to W x H. */
static raster_rig_t*
rig_open(bool with_box, bool detached) {
    raster_rig_t* r = raster_rig_open(wall_and_box, with_box ? 2 : 1, 1U << 1, W, H, sizeof(motion_own_t));
    motion_own_t* own = own_of(r);
    own->attachment = raster_motion_attachment(&own->motion);
    own->attached[0] = &own->attachment;
    if (!detached) {
        raster_rig_attach(r, own->attached, 1);
    }
    return r;
}

static void
draw(raster_rig_t* r, const pose_t* pose, int w, int h) {
    if (pose->box != NULL) {
        r->placement[1] = *pose->box;
    }
    r->raster.width = w;
    r->raster.height = h;
    raster_draw(&r->raster, &pose->camera, 0);
}

static const raster_motion_px_t*
motion_of(const raster_rig_t* r) {
    return raster_rig_attachment(r, 0);
}

static camera_t
camera_at(vec3f_t eye, float yaw_degrees) {
    const float a = yaw_degrees * 3.14159265F / 180.0F;
    return (camera_t){eye, {sinf(a), 0.0F, -cosf(a)}, 0.5F, 1.0F};
}

/* Whether pixel (x, y) and its 3x3 neighbourhood show surface `what`, and
 * its true motion is in what the attachment can hold. */
static bool
well_inside(const pose_t* now, const pose_t* before, int w, int h, int x, int y, float* mx, float* my) {
    const int what = truth(now, before, w, h, x, y, mx, my);
    if (what == NONE || fabsf(*mx) > 60.0F || fabsf(*my) > 60.0F) {
        return false;
    }
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            float ignored;
            if (truth(now, before, w, h, x + dx, y + dy, &ignored, &ignored) != what) {
                return false;
            }
        }
    }
    return true;
}

/* Every pixel well inside one surface holds the true motion within SLACK;
 * returns how many were checked. */
static int
assert_motion_is_true(const raster_rig_t* r, const pose_t* now, const pose_t* before, int w, int h) {
    const raster_motion_px_t* m = motion_of(r);
    int checked = 0;
    float worst = 0.0F;
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            float mx;
            float my;
            if (!well_inside(now, before, w, h, x, y, &mx, &my)) {
                continue;
            }
            const raster_motion_px_t got = m[(y * w) + x];
            TEST_ASSERT_NOT_EQUAL_MESSAGE(RASTER_MOTION_UNKNOWN, got.dx, "a drawn pixel has no motion");
            const float ex = fabsf(((float)got.dx * 0.5F) - mx);
            const float ey = fabsf(((float)got.dy * 0.5F) - my);
            worst = ex > worst ? ex : worst;
            worst = ey > worst ? ey : worst;
            checked++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(worst <= SLACK, "a pixel's motion is off the reprojected truth by more than the slack");
    return checked;
}

static void
test_the_first_picture_knows_no_motion(void) {
    raster_rig_t* r = rig_open(false, false);
    const pose_t now = {camera_at((vec3f_t){0.0F, 0.0F, 300.0F}, 0.0F), NULL};
    draw(r, &now, W, H);
    const raster_motion_px_t* m = motion_of(r);
    for (int i = 0; i < W * H; i++) {
        TEST_ASSERT_EQUAL_INT8(RASTER_MOTION_UNKNOWN, m[i].dx);
    }
}

static void
test_a_still_camera_and_scene_have_no_motion(void) {
    raster_rig_t* r = rig_open(true, false);
    const r3d_placement_t at = turned(20.0F, (vec3f_t){0.0F, 0.0F, 100.0F});
    const pose_t pose = {camera_at((vec3f_t){0.0F, 0.0F, 300.0F}, 0.0F), &at};
    draw(r, &pose, W, H);
    draw(r, &pose, W, H);
    const raster_motion_px_t* m = motion_of(r);
    for (int i = 0; i < W * H; i++) {
        TEST_ASSERT_EQUAL_INT8(0, m[i].dx);
        TEST_ASSERT_EQUAL_INT8(0, m[i].dy);
    }
}

/* One change between two pictures. Before, the box stands unturned at
 * (0, 0, 100) and the camera at (0, 0, 300) looks down -z; now the box is
 * turned `turn` degrees about y and moved by `box`, and the camera moved by
 * `eye` and turned `yaw` degrees. */
typedef struct {
    float turn;
    vec3f_t box;
    vec3f_t eye;
    float yaw;
} change_t;

typedef struct {
    r3d_placement_t box[2];
    pose_t before, now;
} posed_t;

static void
pose_change(const change_t* c, bool with_box, posed_t* p) {
    const vec3f_t box = {0.0F, 0.0F, 100.0F};
    const vec3f_t eye = {0.0F, 0.0F, 300.0F};
    p->box[0] = turned(0.0F, box);
    p->box[1] = turned(c->turn, vec3f_add(box, c->box));
    p->before = (pose_t){camera_at(eye, 0.0F), with_box ? &p->box[0] : NULL};
    p->now = (pose_t){camera_at(vec3f_add(eye, c->eye), c->yaw), with_box ? &p->box[1] : NULL};
}

/* The picture before at `w0` x `h0`, then now at W x H. */
static void
draw_change(raster_rig_t* r, const posed_t* p, int w0, int h0) {
    draw(r, &p->before, w0, h0);
    draw(r, &p->now, W, H);
}

/* Draws `c` on a rig, the box in it when `with_box`, and checks every pixel
 * against the truth; returns how many were checked. */
static int
check_change(const change_t* c, bool with_box, int w0, int h0, posed_t* p) {
    raster_rig_t* r = rig_open(with_box, false);
    pose_change(c, with_box, p);
    draw_change(r, p, w0, h0);
    return assert_motion_is_true(r, &p->now, &p->before, W, H);
}

static void
test_camera_motion_is_the_reprojection_through_the_previous_pose(void) {
    static const change_t c = {0.0F, {0.0F, 0.0F, 0.0F}, {12.0F, -10.0F, -20.0F}, 3.0F};
    posed_t p;
    TEST_ASSERT_GREATER_THAN_INT(W * H * 3 / 4, check_change(&c, false, W, H, &p));
}

static void
test_a_moving_instance_moves_by_its_previous_placement(void) {
    static const change_t c = {15.0F, {14.0F, 4.0F, 10.0F}, {0.0F, 0.0F, 0.0F}, 0.0F};
    posed_t p;
    check_change(&c, true, W, H, &p);
    float mx;
    float my;
    TEST_ASSERT_EQUAL_INT(BOX, truth(&p.now, &p.before, W, H, W / 2, H / 2, &mx, &my));
    TEST_ASSERT_TRUE_MESSAGE(fabsf(mx) > 1.0F, "the box moved on screen");
    TEST_ASSERT_EQUAL_INT8(0, motion_of(raster_rig_now)[2 * W + 2].dx); /* the wall behind it did not */
}

static void
test_camera_and_instance_motion_add_up(void) {
    static const change_t c = {20.0F, {-10.0F, 5.0F, 15.0F}, {15.0F, -5.0F, -15.0F}, 4.0F};
    posed_t p;
    check_change(&c, true, W, H, &p);
}

/* The previous picture was half the size: motion is still in this one's pixels. */
static void
test_a_size_change_between_pictures_keeps_motion_in_this_pictures_pixels(void) {
    static const change_t c = {12.0F, {5.0F, 0.0F, 0.0F}, {8.0F, 0.0F, -10.0F}, 1.5F};
    posed_t p;
    check_change(&c, true, W / 2, H / 2, &p);
}

/* Attaching motion changes no colour and no depth. */
static void
test_motion_leaves_colour_and_depth_as_they_are(void) {
    static const change_t c = {30.0F, {10.0F, 0.0F, 20.0F}, {4.0F, 2.0F, -10.0F}, 1.0F};
    posed_t p;
    pose_change(&c, true, &p);
    uint16_t* plain = malloc(sizeof(uint16_t) * 2 * W * H);
    TEST_ASSERT_NOT_NULL(plain);
    raster_rig_t* r = rig_open(true, true);
    draw_change(r, &p, W, H);
    memcpy(plain, raster_color(&r->raster), sizeof(uint16_t) * W * H);
    memcpy(plain + (W * H), raster_depth(&r->raster), sizeof(uint16_t) * W * H);
    raster_rig_release();
    r = rig_open(true, false);
    draw_change(r, &p, W, H);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(plain, raster_color(&r->raster), W * H);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(plain + (W * H), raster_depth(&r->raster), W * H);
    free(plain);
}

static void
test_forgetting_makes_the_next_picture_first(void) {
    raster_rig_t* r = rig_open(false, false);
    const pose_t pose = {camera_at((vec3f_t){0.0F, 0.0F, 300.0F}, 0.0F), NULL};
    draw(r, &pose, W, H);
    raster_motion_forget(&own_of(r)->motion);
    draw(r, &pose, W, H);
    TEST_ASSERT_EQUAL_INT8(RASTER_MOTION_UNKNOWN, motion_of(r)[(H / 2 * W) + (W / 2)].dx);
}

void
run_raster_motion_suite(void) {
    RUN_TEST(test_the_first_picture_knows_no_motion);
    RUN_TEST(test_a_still_camera_and_scene_have_no_motion);
    RUN_TEST(test_camera_motion_is_the_reprojection_through_the_previous_pose);
    RUN_TEST(test_a_moving_instance_moves_by_its_previous_placement);
    RUN_TEST(test_camera_and_instance_motion_add_up);
    RUN_TEST(test_a_size_change_between_pictures_keeps_motion_in_this_pictures_pixels);
    RUN_TEST(test_motion_leaves_colour_and_depth_as_they_are);
    RUN_TEST(test_forgetting_makes_the_next_picture_first);
}

SUITE_REGISTER(run_raster_motion_suite);
