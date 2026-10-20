/* Geometry figure inputs and library results, emitted as JSON for geometry_figures.py. */
#include <stdbool.h>
#include <stdio.h>

#include "math/geometry/recti.h"
#include "math/geometry/spheref.h"
#include "math/linear/mat4f.h"
#include "math/linear/vec2i.h"

static const recti_t RECT_A = {1, 1, 5, 4};
static const recti_t RECT_B = {3, 2, 5, 4};
static const int RECT_CROSSED_EDGES[] = {8, 7, 6, 6};
static const vec2i_t RECT_SAMPLES[] = {{1, 3}, {3, 1}, {6, 3}, {3, 5}};
static const vec3f_t DISTANCE_NORMAL = {2.0F, 1.0F, 2.0F};
static const vec3f_t DISTANCE_POINT = {0.0F, 0.0F, 1.0F};
static const vec3f_t TRIANGLE[] = {{-1.0F, 0.0F, 0.0F}, {1.0F, 0.3F, 0.0F}, {0.0F, 0.7F, 2.0F}};
static const unsigned WINDING_ORDERS[][3] = {{0, 1, 2}, {0, 2, 1}};
static const vec3f_t WORLD_UP = {0.0F, 1.0F, 0.0F};
static const float SHOWN_DEPTH = 4.5F;
static const vec3f_t LENS_SAMPLE = {0.0F, 0.0F, 3.0F};
static const float LEVEL_VALUES[] = {-1.0F, 1.0F};
static const float SHEET_RADIUS = 2.0F;
static const vec3f_t DISTANCE_SAMPLES[] = {{-1, -1, -1}, {0, 0, 1}, {1, 1, 2}};
static const vec3f_t FRUSTUM_SAMPLES[] = {{0, 0, 4}, {4, 2, 0}};
static const vec2f_t SHEET_SIGNS[] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

static const mat4f_t FRUSTUM_VIEW = {{{0.8660254F, 0, -0.5F, 0}, {0, 1, 0, 0}, {0.5F, 0, 0.8660254F, 0}, {0, 0, 0, 1}}};
static const projectf_t FRUSTUM_LENS = {1, 1, 0, 0, 2};
static const rectf_t FRUSTUM_SCREEN = {-1, -1, 2, 2};
static const box3f_t FRUSTUM_BOXES[] = {
    {{-0.5F, -0.4F, 3.5F}, {0.5F, 0.4F, 4.5F}},  {{-1.5F, -0.4F, 3.5F}, {-0.5F, 0.4F, 4.5F}},
    {{3.5F, -0.4F, 0.8F}, {4.5F, 0.4F, 1.8F}},   {{4.45F, -0.6F, -0.3F}, {5.65F, 0.6F, 0.9F}},
    {{-3.5F, -0.4F, 1.8F}, {-2.5F, 0.4F, 2.5F}}, {{-0.6F, -0.6F, 0.4F}, {0.6F, 0.6F, 1.6F}},
    {{-0.5F, -0.5F, 1.8F}, {0.5F, 0.5F, 2.8F}},  {{2.3F, 2.3F, 0.0F}, {3.5F, 3.5F, 0.8F}},
};

enum { FRUSTUM_LOOSE_BOX = 7 };

_Static_assert(FRUSTUM_LOOSE_BOX < sizeof FRUSTUM_BOXES / sizeof FRUSTUM_BOXES[0], "loose box indexes FRUSTUM_BOXES");
static const box3f_t QUERY_BOX = {{-1, -1, -1}, {1, 1, 1}};
static const spheref_t QUERY_SPHERES[] = {{{1.5F, 0, -1.6F}, 1}, {{2, 0, 0.5F}, 1}, {{2.8F, 0, 2.3F}, 0.7F}};
static const char* const QUERY_SPHERE_NAMES[] = {"overlap", "touch", "miss"};
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
print_sheet(vec3f_t centre, vec3f_t tangent, vec3f_t bitangent, const mat4f_t* matrix) {
    for (size_t i = 0; i < sizeof SHEET_SIGNS / sizeof SHEET_SIGNS[0]; i++) {
        const vec3f_t delta = vec3f_scale(
            vec3f_add(vec3f_scale(tangent, SHEET_SIGNS[i].x), vec3f_scale(bitangent, SHEET_SIGNS[i].y)), SHEET_RADIUS);
        printf("%s", i ? "," : "");
        const vec3f_t point = vec3f_add(centre, delta);
        print_vec(matrix ? mat4f_apply(matrix, point) : point);
    }
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
print_box_frustum(const planef_t planes[PLANE_FRUSTUM_COUNT]) {
    printf(",\"box_frustum\":{\"loose_box\":%d,\"boxes\":[", FRUSTUM_LOOSE_BOX);
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
        printf(",%s,\"%s\"]", spheref_overlaps_box(sphere, QUERY_BOX) ? "true" : "false", QUERY_SPHERE_NAMES[i]);
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
    printf("]},\"distance\":{\"planes\":[");
    print_plane(raw);
    printf(",");
    print_plane(unit);
    printf("],\"sheet\":[");
    const vec3f_t tangent = vec3f_normalize(vec3f_cross(unit.normal, WORLD_UP));
    const vec3f_t bitangent = vec3f_cross(unit.normal, tangent);
    print_sheet(DISTANCE_POINT, tangent, bitangent, NULL);
    printf("],\"levels\":[");
    for (size_t i = 0; i < sizeof LEVEL_VALUES / sizeof LEVEL_VALUES[0]; i++) {
        printf("%s%.7g", i ? "," : "", (double)LEVEL_VALUES[i]);
    }
    printf("],\"level_sheets\":[");
    const planef_t distance_planes[] = {raw, unit};
    for (size_t column = 0; column < sizeof distance_planes / sizeof distance_planes[0]; column++) {
        const vec3f_t normal = distance_planes[column].normal;
        printf("%s[", column ? "," : "");
        for (size_t level = 0; level < sizeof LEVEL_VALUES / sizeof LEVEL_VALUES[0]; level++) {
            const vec3f_t offset = vec3f_scale(normal, LEVEL_VALUES[level] / vec3f_dot(normal, normal));
            printf("%s[", level ? "," : "");
            print_sheet(vec3f_add(DISTANCE_POINT, offset), tangent, bitangent, NULL);
            printf("]");
        }
        printf("]");
    }
    printf("],\"point\":");
    print_vec(DISTANCE_POINT);
    printf(",\"samples\":[");
    for (size_t i = 0; i < sizeof DISTANCE_SAMPLES / sizeof DISTANCE_SAMPLES[0]; i++) {
        printf("%s[", i ? "," : "");
        print_vec(DISTANCE_SAMPLES[i]);
        printf(",%.7g,%.7g]", (double)planef_distance(raw, DISTANCE_SAMPLES[i]),
               (double)planef_distance(unit, DISTANCE_SAMPLES[i]));
    }
    printf("]},\"triangle\":{\"points\":[");
    for (size_t i = 0; i < sizeof TRIANGLE / sizeof TRIANGLE[0]; i++) {
        printf("%s", i ? "," : "");
        print_vec(TRIANGLE[i]);
    }
    const vec3f_t centroid = vec3f_scale(vec3f_add(vec3f_add(TRIANGLE[0], TRIANGLE[1]), TRIANGLE[2]),
                                         1.0F / (float)(sizeof TRIANGLE / sizeof TRIANGLE[0]));
    printf("],\"centroid\":");
    print_vec(centroid);
    printf(",\"normals\":[");
    for (size_t i = 0; i < sizeof WINDING_ORDERS / sizeof WINDING_ORDERS[0]; i++) {
        const unsigned* order = WINDING_ORDERS[i];
        printf("%s", i ? "," : "");
        print_vec(planef_from_points(TRIANGLE[order[0]], TRIANGLE[order[1]], TRIANGLE[order[2]]).normal);
    }
    printf("],\"orders\":[");
    for (size_t i = 0; i < sizeof WINDING_ORDERS / sizeof WINDING_ORDERS[0]; i++) {
        printf("%s[%u,%u,%u]", i ? "," : "", WINDING_ORDERS[i][0], WINDING_ORDERS[i][1], WINDING_ORDERS[i][2]);
    }
    printf("]},\"frustum\":{");

    const mat4f_t lens = mat4f_scale_rows(FRUSTUM_VIEW, (vec3f_t){FRUSTUM_LENS.fx, -FRUSTUM_LENS.fy, 1.0F});
    const mat4f_t inverse = mat4f_invert_affine(lens);
    planef_t planes[PLANE_FRUSTUM_COUNT];
    planef_frustum(&FRUSTUM_VIEW, &FRUSTUM_LENS, FRUSTUM_SCREEN, planes);
    printf("\"plane_count\":%d,\"camera\":", PLANE_FRUSTUM_COUNT);
    print_vec(mat4f_apply(&inverse, (vec3f_t){0}));
    printf(",\"near\":%.7g,\"shown_depth\":%.7g,\"corners\":[", (double)FRUSTUM_LENS.near_z, (double)SHOWN_DEPTH);
    const vec2f_t picture[] = {{FRUSTUM_SCREEN.x, FRUSTUM_SCREEN.y},
                               {rectf_right(FRUSTUM_SCREEN), FRUSTUM_SCREEN.y},
                               {rectf_right(FRUSTUM_SCREEN), rectf_bottom(FRUSTUM_SCREEN)},
                               {FRUSTUM_SCREEN.x, rectf_bottom(FRUSTUM_SCREEN)}};
    const float depths[] = {FRUSTUM_LENS.near_z, SHOWN_DEPTH};
    bool comma = false;
    for (size_t k = 0; k < sizeof depths / sizeof depths[0]; k++) {
        for (size_t i = 0; i < sizeof picture / sizeof picture[0]; i++) {
            const float depth = depths[k];
            const vec3f_t p = {(picture[i].x - FRUSTUM_LENS.cx) * depth, (picture[i].y - FRUSTUM_LENS.cy) * depth,
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
    printf("],\"samples\":[");
    for (size_t k = 0; k < sizeof FRUSTUM_SAMPLES / sizeof FRUSTUM_SAMPLES[0]; k++) {
        const vec3f_t point = FRUSTUM_SAMPLES[k];
        unsigned mask = (1U << PLANE_FRUSTUM_COUNT) - 1U;
        const bool inside = !box3f_outside_planes((box3f_t){point, point}, planes, PLANE_FRUSTUM_COUNT, &mask);
        printf("%s[", k ? "," : "");
        print_vec(point);
        printf(",%s]", inside ? "true" : "false");
    }
    const planef_t lens_plane = {{1.0F, 0.0F, FRUSTUM_LENS.cx - FRUSTUM_SCREEN.x}, 0.0F};
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
    printf(",\"sheets\":[");
    const mat4f_t* sheet_spaces[] = {NULL, &inverse};
    const vec3f_t sheet_centre = {-lens_plane.normal.z * LENS_SAMPLE.z, 0, LENS_SAMPLE.z};
    for (size_t space = 0; space < sizeof sheet_spaces / sizeof sheet_spaces[0]; space++) {
        printf("%s[", space ? "," : "");
        print_sheet(sheet_centre, (vec3f_t){-lens_plane.normal.z, 0, 1}, (vec3f_t){0, 1, 0}, sheet_spaces[space]);
        printf("]");
    }
    printf("]");
    printf(",\"feet\":[");
    const planef_t row_planes[] = {lens_plane, world_plane};
    const vec3f_t row_samples[] = {LENS_SAMPLE, world_sample};
    for (size_t i = 0; i < sizeof row_planes / sizeof row_planes[0]; i++) {
        const vec3f_t normal = row_planes[i].normal;
        const float scale = planef_distance(row_planes[i], row_samples[i]) / vec3f_dot(normal, normal);
        printf("%s", i ? "," : "");
        print_vec(vec3f_sub(row_samples[i], vec3f_scale(normal, scale)));
    }
    printf("]");
    printf(",\"distances\":[%.7g,%.7g]}", (double)planef_distance(lens_plane, LENS_SAMPLE),
           (double)planef_distance(world_plane, world_sample));
    print_box_frustum(planes);
    print_queries();
    printf("}\n");
    return 0;
}
