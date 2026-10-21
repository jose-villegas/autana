/* Geometry figure inputs and library results, emitted as JSON for geometry_figures.py. */
#include <stdbool.h>
#include <stdio.h>

#include "math/geometry/recti.h"
#include "math/geometry/spheref.h"
#include "math/linear/transformf.h"
#include "math/linear/vec2i.h"

static const recti_t RECT_A = {1, 1, 5, 4};
static const recti_t RECT_B = {3, 2, 5, 4};
static const int RECT_CROSSED_EDGES[] = {8, 7, 6, 6};
static const vec2i_t RECT_SAMPLES[] = {{1, 3}, {3, 1}, {6, 3}, {3, 5}};
static const vec3f_t DISTANCE_NORMAL = {2.0F, 0.0F, 2.0F};
static const vec3f_t DISTANCE_POINT = {0.0F, 0.0F, 1.0F};
static const vec3f_t TRIANGLE[] = {{-1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 2.0F}};
static const vec3f_t CAMERA_POSITION = {-5.0F, 5.0F, -6.0F};
static const vec3f_t CAMERA_TARGET = {0.0F, 0.0F, 0.0F};
static const vec3f_t WORLD_UP = {0.0F, 1.0F, 0.0F};
static const float LENS_FOCAL = 110.0F;
static const float NEAR_DEPTH = 2.0F;
static const float SHOWN_DEPTH = 11.0F;
static const vec2f_t PICTURE_CENTER = {60.0F, 40.0F};
static const float SCREEN_LEFT = 0.0F, SCREEN_TOP = 0.0F, SCREEN_RIGHT = 120.0F, SCREEN_BOTTOM = 80.0F;
static const vec3f_t LENS_SAMPLE = {0.0F, 0.0F, 3.0F};
static const float GRID_MIN = -8.0F, GRID_MAX = 12.0F, GRID_STEP = 1.0F;
static const float BAND_MIN = -2.0F, BAND_MAX = 3.0F, BAND_STEP = 0.1F;

static const mat4f_t BOX_VIEW = {{{0.8660254F, 0, -0.5F, 0}, {0, 1, 0, 0}, {0.5F, 0, 0.8660254F, 0}, {0, 0, 0, 1}}};
static const projectf_t BOX_LENS = {1, 1, 0, 0, 2};
static const rectf_t BOX_SCREEN = {-1, -1, 2, 2};
static const box3f_t FRUSTUM_BOXES[] = {
    {{-0.5F, -0.1F, 3.5F}, {0.5F, 0.1F, 4.5F}},  {{-1.5F, -0.1F, 3.5F}, {-0.5F, 0.1F, 4.5F}},
    {{3.5F, -0.1F, 0.8F}, {4.5F, 0.1F, 1.8F}},   {{4.8F, -0.1F, 0.1F}, {5.3F, 0.1F, 0.5F}},
    {{-3.5F, -0.1F, 1.8F}, {-2.5F, 0.1F, 2.5F}}, {{-0.1F, -0.1F, 0.8F}, {0.1F, 0.1F, 1.2F}},
    {{-0.3F, -0.1F, 2.0F}, {0.3F, 0.1F, 2.6F}},  {{2.5F, -0.1F, 0.6F}, {3.0F, 0.1F, 0.72F}},
};
static const box3f_t QUERY_BOX = {{-1, -1, -1}, {1, 1, 1}};
static const spheref_t QUERY_SPHERES[] = {{{1.5F, 0, -1.6F}, 1}, {{2, 0, 0.5F}, 1}, {{2.8F, 0, 2.3F}, 0.7F}};
static const spheref_t SEGMENT_SPHERE = {{3, 0, 0}, 1};

enum { QUERY_BOX_TARGET, QUERY_SPHERE_TARGET, QUERY_TARGET_COUNT };

static const vec3f_t QUERY_SEGMENTS[][2] = {
    {{-3, 0, -0.5F}, {1.5F, 0, -0.5F}},
    {{1.5F, 0, -2}, {4, 0, 2}},
    {{3, 0, 0}, {5, 0, 1}},
    {{-3, 0, 2}, {5, 0, 2}},
};

static void
print_vec(vec3f_t p) {
    printf("[%.7g,%.7g,%.7g]", (double)p.x, (double)p.y, (double)p.z);
}

static void
print_plane(planef_t p) {
    printf("[%.7g,%.7g,%.7g,%.7g]", (double)p.normal.x, (double)p.normal.y, (double)p.normal.z, (double)p.d);
}

static void
print_rect(recti_t r) {
    printf("[%d,%d,%d,%d]", r.x, r.y, r.w, r.h);
}

static void
print_box(box3f_t b) {
    printf("[");
    print_vec(b.lo);
    printf(",");
    print_vec(b.hi);
    printf("]");
}

static void
print_sphere(spheref_t sphere) {
    printf("[");
    print_vec(sphere.centre);
    printf(",%.7g]", (double)sphere.radius);
}

static void
print_box_frustum(void) {
    planef_t planes[PLANE_FRUSTUM_COUNT];
    planef_frustum(&BOX_VIEW, &BOX_LENS, BOX_SCREEN, planes);
    printf(",\"box_frustum\":{\"planes\":[");
    for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {
        printf("%s", p ? "," : "");
        print_plane(planes[p]);
    }
    printf("],\"boxes\":[");
    for (size_t i = 0; i < sizeof FRUSTUM_BOXES / sizeof FRUSTUM_BOXES[0]; i++) {
        unsigned mask = (1U << PLANE_FRUSTUM_COUNT) - 1U;
        const bool outside = box3f_outside_planes(FRUSTUM_BOXES[i], planes, PLANE_FRUSTUM_COUNT, &mask);
        printf("%s[", i ? "," : "");
        print_box(FRUSTUM_BOXES[i]);
        printf(",%s,%u]", outside ? "true" : "false", mask);
    }
    printf("]}");
}

static void
print_queries(void) {
    printf(",\"queries\":{\"box\":");
    print_box(QUERY_BOX);
    printf(",\"spheres\":[");
    for (size_t i = 0; i < sizeof QUERY_SPHERES / sizeof QUERY_SPHERES[0]; i++) {
        const spheref_t sphere = QUERY_SPHERES[i];
        printf("%s[", i ? "," : "");
        print_sphere(sphere);
        printf(",");
        print_vec(box3f_closest_point(QUERY_BOX, sphere.centre));
        printf(",%s]", spheref_overlaps_box(sphere, QUERY_BOX) ? "true" : "false");
    }
    printf("],\"segment_sphere\":");
    print_sphere(SEGMENT_SPHERE);
    printf(",\"segments\":[");
    for (size_t i = 0; i < sizeof QUERY_SEGMENTS / sizeof QUERY_SEGMENTS[0]; i++) {
        const vec3f_t a = QUERY_SEGMENTS[i][0], c = QUERY_SEGMENTS[i][1];
        printf("%s[", i ? "," : "");
        print_vec(a);
        printf(",");
        print_vec(c);
        printf(",[");
        for (unsigned target = 0; target < QUERY_TARGET_COUNT; target++) {
            float t = 0.0F;
            const bool hit =
                target ? spheref_segment_hit(SEGMENT_SPHERE, a, c, &t) : box3f_segment_hit(QUERY_BOX, a, c, &t);
            printf("%s[%s,%.7g,", target ? "," : "", hit ? "true" : "false", (double)t);
            print_vec(vec3f_lerp(a, c, t));
            printf("]");
        }
        printf("]]");
    }
    printf("]}");
}

int
main(void) {
    const recti_t overlap = recti_intersect(RECT_A, RECT_B);
    const recti_t combined = recti_union(RECT_A, RECT_B);
    const recti_t crossed =
        recti_from_edges(RECT_CROSSED_EDGES[0], RECT_CROSSED_EDGES[1], RECT_CROSSED_EDGES[2], RECT_CROSSED_EDGES[3]);
    printf("{\"rect\":{\"a\":");
    print_rect(RECT_A);
    printf(",\"b\":");
    print_rect(RECT_B);
    printf(",\"intersect\":");
    print_rect(overlap);
    printf(",\"union\":");
    print_rect(combined);
    printf(",\"crossed\":");
    print_rect(crossed);
    printf(",\"empty\":%s,\"edges\":[", recti_is_empty(crossed) ? "true" : "false");
    for (size_t i = 0; i < sizeof RECT_CROSSED_EDGES / sizeof RECT_CROSSED_EDGES[0]; i++) {
        printf("%s%d", i ? "," : "", RECT_CROSSED_EDGES[i]);
    }
    printf("],\"samples\":[");
    for (size_t i = 0; i < sizeof RECT_SAMPLES / sizeof RECT_SAMPLES[0]; i++) {
        printf("%s[%d,%d,%s]", i ? "," : "", RECT_SAMPLES[i].x, RECT_SAMPLES[i].y,
               recti_contains(RECT_A, RECT_SAMPLES[i].x, RECT_SAMPLES[i].y) ? "true" : "false");
    }
    const planef_t raw = planef_from_normal_point(DISTANCE_NORMAL, DISTANCE_POINT);
    const planef_t unit = planef_normalize(raw);
    printf("]},\"distance\":{\"normal_length\":%.7g,\"grid\":[",
           (double)mathf_sqrt(vec3f_dot(DISTANCE_NORMAL, DISTANCE_NORMAL)));
    bool comma = false;
    for (float z = BAND_MIN; z <= BAND_MAX; z += BAND_STEP) {
        for (float x = BAND_MIN; x <= BAND_MAX; x += BAND_STEP) {
            const vec3f_t p = {x, 0.0F, z};
            printf("%s[%.7g,%.7g,%.7g,%.7g]", comma ? "," : "", (double)x, (double)z, (double)planef_distance(raw, p),
                   (double)planef_distance(unit, p));
            comma = true;
        }
    }
    printf("]},\"triangle\":{\"points\":[");
    for (size_t i = 0; i < sizeof TRIANGLE / sizeof TRIANGLE[0]; i++) {
        printf("%s", i ? "," : "");
        print_vec(TRIANGLE[i]);
    }
    const planef_t forward = planef_from_points(TRIANGLE[0], TRIANGLE[1], TRIANGLE[2]);
    const planef_t reverse = planef_from_points(TRIANGLE[0], TRIANGLE[2], TRIANGLE[1]);
    const vec3f_t centroid = vec3f_scale(vec3f_add(vec3f_add(TRIANGLE[0], TRIANGLE[1]), TRIANGLE[2]),
                                         1.0F / (float)(sizeof TRIANGLE / sizeof TRIANGLE[0]));
    printf("],\"centroid\":");
    print_vec(centroid);
    printf(",\"normals\":[");
    print_vec(forward.normal);
    printf(",");
    print_vec(reverse.normal);
    printf("]},\"frustum\":{");

    transformf_t camera = TRANSFORMF_IDENTITY;
    transformf_set_position(&camera, CAMERA_POSITION);
    transformf_look_at(&camera, CAMERA_TARGET, WORLD_UP);
    const mat4f_t view = transformf_view(&camera);
    const mat4f_t lens = mat4f_scale_rows(view, (vec3f_t){LENS_FOCAL, -LENS_FOCAL, 1.0F});
    const mat4f_t inverse = mat4f_invert_affine(lens);
    const rectf_t screen = rectf_from_edges(SCREEN_LEFT, SCREEN_TOP, SCREEN_RIGHT, SCREEN_BOTTOM);
    planef_t planes[PLANE_FRUSTUM_COUNT];
    const projectf_t pinhole = {LENS_FOCAL, LENS_FOCAL, PICTURE_CENTER.x, PICTURE_CENTER.y, NEAR_DEPTH};
    planef_frustum(&view, &pinhole, screen, planes);
    printf("\"camera\":");
    print_vec(CAMERA_POSITION);
    printf(",\"near\":%.7g,\"shown_depth\":%.7g,\"corners\":[", (double)NEAR_DEPTH, (double)SHOWN_DEPTH);
    const vec2f_t picture[] = {{screen.x, screen.y},
                               {rectf_right(screen), screen.y},
                               {rectf_right(screen), rectf_bottom(screen)},
                               {screen.x, rectf_bottom(screen)}};
    const float depths[] = {NEAR_DEPTH, SHOWN_DEPTH};
    comma = false;
    for (size_t k = 0; k < sizeof depths / sizeof depths[0]; k++) {
        for (size_t i = 0; i < sizeof picture / sizeof picture[0]; i++) {
            const float depth = depths[k];
            const vec3f_t p = {(picture[i].x - PICTURE_CENTER.x) * depth, (picture[i].y - PICTURE_CENTER.y) * depth,
                               depth};
            printf("%s", comma ? "," : "");
            print_vec(mat4f_apply(&inverse, p));
            comma = true;
        }
    }
    printf("],\"planes\":[");
    for (int i = 0; i < PLANE_FRUSTUM_COUNT; i++) {
        printf("%s", i ? "," : "");
        print_plane(planes[i]);
    }
    printf("],\"grid\":[");
    comma = false;
    for (float z = GRID_MIN; z <= GRID_MAX; z += GRID_STEP) {
        for (float x = GRID_MIN; x <= GRID_MAX; x += GRID_STEP) {
            const vec3f_t p = {x, 0.0F, z};
            bool inside = true;
            printf("%s[%.7g,%.7g,[", comma ? "," : "", (double)x, (double)z);
            for (int i = 0; i < PLANE_FRUSTUM_COUNT; i++) {
                const float distance = planef_distance(planes[i], p);
                inside = inside && distance >= 0.0F;
                printf("%s%.7g", i ? "," : "", (double)distance);
            }
            printf("],%s]", inside ? "true" : "false");
            comma = true;
        }
    }
    const planef_t lens_plane = {{1.0F, 0.0F, PICTURE_CENTER.x - SCREEN_LEFT}, 0.0F};
    const planef_t world_plane =
        planef_from_rows(&lens, lens_plane.normal.x, lens_plane.normal.y, lens_plane.normal.z, lens_plane.d);
    const vec3f_t world_sample = mat4f_apply(&inverse, LENS_SAMPLE);
    printf("]},\"rows\":{\"lens_plane\":");
    print_plane(lens_plane);
    printf(",\"world_plane\":");
    print_plane(world_plane);
    printf(",\"lens_sample\":");
    print_vec(LENS_SAMPLE);
    printf(",\"world_sample\":");
    print_vec(world_sample);
    printf(",\"distances\":[%.7g,%.7g]}", (double)planef_distance(lens_plane, LENS_SAMPLE),
           (double)planef_distance(world_plane, world_sample));
    print_box_frustum();
    print_queries();
    printf("}\n");
    return 0;
}
