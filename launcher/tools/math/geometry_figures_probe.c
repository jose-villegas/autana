/* Geometry figure inputs and library results, emitted as JSON for geometry_figures.py. */
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "math/geometry/circlef.h"
#include "math/geometry/circlei.h"
#include "math/geometry/recti.h"
#include "math/geometry/spheref.h"
#include "math/linear/mat4f.h"
#include "math/linear/vec2i.h"
#include "math/text/math_text.h"

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
static const vec3f_t FRUSTUM_SAMPLES[] = {{0, 0, 4}, {-3, -1, 1}};
static const vec2f_t SHEET_SIGNS[] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

static const mat4f_t FRUSTUM_VIEW = {{{0.8660254F, 0, -0.5F, 0}, {0, 1, 0, 0}, {0.5F, 0, 0.8660254F, 0}, {0, 0, 0, 1}}};
static const projectf_t FRUSTUM_LENS = {1, 1, 0, 0, 2};
static const rectf_t FRUSTUM_SCREEN = {-1, -1, 2, 2};
static const box3f_t FRUSTUM_BOXES[] = {
    {{-6.00F, 1.70F, -2.00F}, {-5.15F, 2.60F, -1.00F}}, {{-3.20F, 0.00F, -1.60F}, {-2.35F, 0.90F, -0.60F}},
    {{-0.40F, 0.00F, -2.00F}, {0.45F, 0.90F, -1.00F}},  {{2.40F, 0.00F, -1.60F}, {3.25F, 0.90F, -0.60F}},
    {{-5.50F, 0.00F, 0.60F}, {-4.65F, 0.90F, 1.60F}},   {{-2.70F, 0.00F, 1.00F}, {-1.85F, 0.90F, 2.00F}},
    {{0.10F, 0.00F, 0.60F}, {0.95F, 0.90F, 1.60F}},     {{2.90F, 1.70F, 1.00F}, {3.75F, 2.60F, 2.00F}},
    {{5.70F, 0.00F, 0.60F}, {6.55F, 0.90F, 1.60F}},     {{-6.00F, 0.00F, 3.20F}, {-5.15F, 0.90F, 4.20F}},
    {{-3.20F, 0.00F, 3.60F}, {-2.35F, 0.90F, 4.60F}},   {{-0.40F, 1.70F, 3.20F}, {0.45F, 2.60F, 4.20F}},
    {{2.40F, 0.00F, 3.60F}, {3.25F, 0.90F, 4.60F}},     {{5.20F, 0.00F, 3.20F}, {6.05F, 0.90F, 4.20F}},
    {{-5.50F, 0.00F, 5.80F}, {-4.65F, 0.90F, 6.80F}},   {{-2.70F, 1.70F, 6.20F}, {-1.85F, 2.60F, 7.20F}},
    {{0.10F, 0.00F, 5.80F}, {0.95F, 0.90F, 6.80F}},     {{2.90F, 0.00F, 6.20F}, {3.75F, 0.90F, 7.20F}},
    {{5.70F, 0.00F, 5.80F}, {6.55F, 0.90F, 6.80F}},     {{-6.00F, 1.70F, 8.40F}, {-5.15F, 2.60F, 9.40F}},
    {{-3.20F, 0.00F, 8.80F}, {-2.35F, 0.90F, 9.80F}},   {{-0.40F, 0.00F, 8.40F}, {0.45F, 0.90F, 9.40F}},
    {{2.40F, 0.00F, 8.80F}, {3.25F, 0.90F, 9.80F}},     {{5.20F, 1.70F, 8.40F}, {6.05F, 2.60F, 9.40F}},
    {{0.30F, 4.30F, -1.00F}, {1.50F, 5.50F, -0.20F}},
};

static const vec3f_t PATH_ORIGIN = {-2.0F, 2.0F, -1.0F};
static const vec3f_t PATH_TRAVEL = {3.0F, 0.0F, 2.0F};
static const float PATH_YAW = 30.0F * MATH_RADIANS_PER_DEGREE;
static const float PATH_TURN = 65.0F * MATH_RADIANS_PER_DEGREE;
static const float PATH_DEPTH_FREQUENCY = 2.0F;
static const float OVERLAP_RADIUS = 1.0F;
static const float OVERLAP_MID_X = 2.0F;
static const float OVERLAP_TRAVEL = 1.5F;

static const box3f_t QUERY_BOX = {{-1, -1, -1}, {1, 1, 1}};
static const spheref_t SEGMENT_SPHERE = {{3, 0, 0}, 1};

enum { QUERY_BOX_TARGET, QUERY_SPHERE_TARGET, QUERY_TARGET_COUNT };

static const vec3f_t QUERY_SEGMENTS[][2] = {
    {{-3, 0, -0.5F}, {1.5F, 0, -0.5F}},
    {{1.5F, 0, -2}, {4, 0, 2}},
    {{3, 0, 0}, {5, 0, 1}},
    {{-3, 0, 2}, {5, 0, 2}},
};

static const recti_t CIRCLE_RECT = {1, 1, 4, 9};
static const circlei_t RECT_CIRCLES[] = {{{5, 2}, 2}, {{5, 5}, 1}, {{6, 8}, 1}};
static const circlef_t SEGMENT_CIRCLE = {{0, 0}, 2};
static const vec2f_t CIRCLE_SEGMENTS[][2] = {
    {{-3, -1}, {3, -1}},
    {{0, 0}, {3, 1}},
    {{-3, 3}, {3, 3}},
};

static void
print_circle_queries(void) {
    const vec2i_t lo = {CIRCLE_RECT.x, CIRCLE_RECT.y};
    const vec2i_t hi = {recti_right(CIRCLE_RECT) - mathi_one(), recti_bottom(CIRCLE_RECT) - mathi_one()};
    printf(",\"circle\":{\"rect\":[%d,%d,%d,%d],\"last\":[%s],\"circles\":[", CIRCLE_RECT.x, CIRCLE_RECT.y,
           CIRCLE_RECT.w, CIRCLE_RECT.h, vec2i_text(hi).chars);
    for (size_t i = 0; i < sizeof RECT_CIRCLES / sizeof RECT_CIRCLES[0]; i++) {
        const circlei_t circle = RECT_CIRCLES[i];
        const vec2i_t closest = vec2i_clamp(circle.centre, lo, hi);
        const vec2i_t delta = vec2i_sub(circle.centre, closest);
        const bool hit = circlei_overlaps_rect(circle, CIRCLE_RECT);
        const bool touch = vec2i_dot(delta, delta) == circle.radius * circle.radius;
        printf("%s[[%s],%d,[%s],%s,\"%s\",[", i ? "," : "", vec2i_text(circle.centre).chars, circle.radius,
               vec2i_text(closest).chars, hit ? "true" : "false", hit ? (touch ? "touch" : "overlap") : "miss");
        for (int y = lo.y; y <= hi.y; y++) {
            for (int x = lo.x; x <= hi.x; x++) {
                const bool inside = circlei_contains_point(circle, (vec2i_t){x, y});
                printf("%s[%d,%d,%s]", x == lo.x && y == lo.y ? "" : ",", x, y, inside ? "true" : "false");
            }
        }
        printf("]]");
    }
    printf("],\"segment_circle\":[");
    printf("[%s]", vec2f_text(SEGMENT_CIRCLE.centre).chars);
    printf(",%.7g],\"segments\":[", (double)SEGMENT_CIRCLE.radius);
    for (size_t i = 0; i < sizeof CIRCLE_SEGMENTS / sizeof CIRCLE_SEGMENTS[0]; i++) {
        const vec2f_t a = CIRCLE_SEGMENTS[i][0], b = CIRCLE_SEGMENTS[i][1];
        float t = 0.0F;
        const bool hit = circlef_segment_hit(SEGMENT_CIRCLE, a, b, &t);
        printf("%s[", i ? "," : "");
        printf("[%s]", vec2f_text(a).chars);
        printf(",");
        printf("[%s]", vec2f_text(b).chars);
        printf(",%s,%.7g,", hit ? "true" : "false", (double)t);
        printf("[%s]", vec2f_text(circlef_closest_point(SEGMENT_CIRCLE, vec2f_lerp(a, b, t))).chars);
        printf("]");
    }
    printf("]}");
}

static void
print_vec(vec3f_t p) {
    printf("[%s]", vec3f_text(p).chars);
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
    printf("[%s,%s]", vec3f_text(p.normal).chars, mathf_text(p.d).chars);
}

static void
print_rect(recti_t r) {
    printf("[%s,%s]", vec2i_text((vec2i_t){r.x, r.y}).chars, vec2i_text((vec2i_t){r.w, r.h}).chars);
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
print_frustum(const mat4f_t* view, planef_t planes[PLANE_FRUSTUM_COUNT]) {
    const mat4f_t lens = mat4f_scale_rows((*view), (vec3f_t){FRUSTUM_LENS.fx, -FRUSTUM_LENS.fy, 1.0F});
    const mat4f_t inverse = mat4f_invert_affine(lens);
    planef_frustum(view, &FRUSTUM_LENS, FRUSTUM_SCREEN, planes);
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
    printf("]");
}

static void
print_box_frustum(int frames) {
    printf(",\"box_frustum\":[");
    for (int frame = 0; frame < frames; frame++) {
        const float phase = MATH_TAU * (float)frame / (float)frames;
        const float yaw = PATH_YAW + PATH_TURN * sinf(phase);
        const vec3f_t camera = vec3f_add(
            PATH_ORIGIN, (vec3f_t){PATH_TRAVEL.x * sinf(phase), 0, PATH_TRAVEL.z * sinf(PATH_DEPTH_FREQUENCY * phase)});
        const mat4f_t view = mat4f_view_from_axes((vec3f_t){cosf(yaw), 0, -sinf(yaw)}, WORLD_UP,
                                                  (vec3f_t){sinf(yaw), 0, cosf(yaw)}, camera);
        planef_t planes[PLANE_FRUSTUM_COUNT];
        printf("%s{\"frustum\":{", frame ? "," : "");
        print_frustum(&view, planes);
        printf("},\"boxes\":[");
        for (size_t i = 0; i < sizeof FRUSTUM_BOXES / sizeof FRUSTUM_BOXES[0]; i++) {
            unsigned mask = (1U << PLANE_FRUSTUM_COUNT) - 1U;
            const bool outside = box3f_outside_planes(FRUSTUM_BOXES[i], planes, PLANE_FRUSTUM_COUNT, &mask);
            printf("%s[", i ? "," : "");
            print_box(FRUSTUM_BOXES[i]);
            printf(",%s,%u]", outside ? "true" : "false", mask);
        }
        printf("]}");
    }
    printf("]");
}

static void
print_queries(int frames) {
    printf(",\"queries\":{\"box\":");
    print_box(QUERY_BOX);
    printf(",\"spheres\":[");
    for (int frame = 0; frame < frames; frame++) {
        const float phase = MATH_TAU * (float)frame / (float)frames;
        const spheref_t sphere = {{OVERLAP_MID_X + OVERLAP_TRAVEL * sinf(phase), 0, 0}, OVERLAP_RADIUS};
        const vec3f_t closest = box3f_closest_point(QUERY_BOX, sphere.centre);
        const bool hit = spheref_overlaps_box(sphere, QUERY_BOX);
        const vec3f_t delta = vec3f_sub(sphere.centre, closest);
        const bool touch = vec3f_dot(delta, delta) == sphere.radius * sphere.radius;
        printf("%s[", frame ? "," : "");
        print_sphere(sphere);
        printf(",");
        print_vec(closest);
        printf(",%s,\"%s\"]", hit ? "true" : "false", hit ? (touch ? "touch" : "overlap") : "miss");
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
main(int argc, char** argv) {
    enum { FRAME_COUNT_ARGUMENT = 1, PROBE_ARGUMENT_COUNT = 2 };

    if (argc != PROBE_ARGUMENT_COUNT) {
        return EXIT_FAILURE;
    }
    char* end;
    const long frame_count = strtol(argv[FRAME_COUNT_ARGUMENT], &end, 10);
    if (*end || frame_count <= 0 || frame_count > INT_MAX) {
        return EXIT_FAILURE;
    }
    const int frames = (int)frame_count;
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
        printf("%s[%s,%s]", i ? "," : "", vec2i_text(RECT_SAMPLES[i]).chars,
               recti_contains(RECT_A, RECT_SAMPLES[i].x, RECT_SAMPLES[i].y) ? "true" : "false");
    }
    const planef_t raw = planef_from_normal_point(DISTANCE_NORMAL, DISTANCE_POINT);
    const planef_t unit = planef_normalize(raw);
    printf("]},\"distance\":{\"planes\":[");
    print_plane(raw);
    printf(",");
    print_plane(unit);
    printf("],\"normal_length\":%.7g,\"rulers\":[", (double)sqrtf(vec3f_dot(raw.normal, raw.normal)));
    const planef_t distance_planes[] = {raw, unit};
    for (size_t column = 0; column < sizeof distance_planes / sizeof distance_planes[0]; column++) {
        const planef_t plane = distance_planes[column];
        const float normal_length = sqrtf(vec3f_dot(plane.normal, plane.normal));
        printf("%s[", column ? "," : "");
        for (size_t level = 0; level < sizeof LEVEL_VALUES / sizeof LEVEL_VALUES[0]; level++) {
            const vec3f_t point =
                vec3f_add(DISTANCE_POINT, vec3f_scale(unit.normal, LEVEL_VALUES[level] / normal_length));
            printf("%s[%.7g,%.7g]", level ? "," : "", (double)(LEVEL_VALUES[level] / normal_length),
                   (double)planef_distance(plane, point));
        }
        printf("]");
    }
    printf("],\"samples\":[");
    const vec3f_t side_up =
        vec3f_normalize(vec3f_sub(WORLD_UP, vec3f_scale(unit.normal, vec3f_dot(WORLD_UP, unit.normal))));
    for (size_t i = 0; i < sizeof DISTANCE_SAMPLES / sizeof DISTANCE_SAMPLES[0]; i++) {
        const vec3f_t point = DISTANCE_SAMPLES[i];
        printf("%s[", i ? "," : "");
        print_vec(point);
        printf(",%.7g,%.7g,%.7g]", (double)planef_distance(raw, point), (double)planef_distance(unit, point),
               (double)vec3f_dot(vec3f_sub(point, DISTANCE_POINT), side_up));
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

    planef_t planes[PLANE_FRUSTUM_COUNT];
    print_frustum(&FRUSTUM_VIEW, planes);
    const mat4f_t lens = mat4f_scale_rows(FRUSTUM_VIEW, (vec3f_t){FRUSTUM_LENS.fx, -FRUSTUM_LENS.fy, 1.0F});
    const mat4f_t inverse = mat4f_invert_affine(lens);
    printf(",\"samples\":[");
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
    print_box_frustum(frames);
    print_queries(frames);
    print_circle_queries();
    printf("}\n");
    return 0;
}
