/*
 * Prints, as JSON, what the maths library does to the example inputs that
 * docs/math/README.md's figures draw: a shape through scale, rotate and
 * translate, and one camera point on its way to a pixel. math_figures.py
 * plots the output, so every number in the figures comes from these calls.
 *
 *     math_figures_probe WIDTH HEIGHT
 *
 * WIDTH and HEIGHT are the viewport the example camera renders into.
 */
#include <stdio.h>
#include <stdlib.h>

#include "math/linear/mat4f.h"
#include "math/linear/math_const.h"
#include "math/linear/projectf.h"
#include "math/linear/quatf.h"
#include "math/linear/transformf.h"
#include "render/r3d_project.h"
#include "render/render_view.h"

/* An L, so a turn or a mirror shows: local units, corners in drawing order. */
static const vec3f_t SHAPE[] = {
    {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 0.4F, 0.0F},
    {0.4F, 0.4F, 0.0F}, {0.4F, 1.5F, 0.0F}, {0.0F, 1.5F, 0.0F},
};
#define SHAPE_CORNERS ((int)(sizeof SHAPE / sizeof SHAPE[0]))

static const vec3f_t TRS_SCALE = {1.5F, 0.75F, 1.0F};
static const vec3f_t TRS_AXIS = {0.0F, 0.0F, 1.0F};
static const float TRS_ANGLE_DEGREES = 30.0F;
static const vec3f_t TRS_POSITION = {2.0F, 1.0F, 0.0F};

/* The example camera sits at the origin looking down +z, as TRANSFORMF_IDENTITY places it. */
static const float VIEW_HALF_FOV_SHORT_TAN = 0.5F;
static const float VIEW_NEAR_Z = 0.1F;
static const vec3f_t VIEW_POINT = {0.62F, 0.41F, 2.0F};

static void
print_shape(const char* name, const mat4f_t* m, const char* tail) {
    printf("  \"%s\": [", name);
    for (int i = 0; i < SHAPE_CORNERS; i++) {
        const vec3f_t p = mat4f_apply(m, SHAPE[i]);
        printf("%s[%.6g, %.6g]", i ? ", " : "", (double)p.x, (double)p.y);
    }
    printf("]%s\n", tail);
}

int
main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s WIDTH HEIGHT\n", argv[0]);
        return 2;
    }
    const viewport_t viewport = {atoi(argv[1]), atoi(argv[2]), 0};

    const vec3f_t origin = {0.0F, 0.0F, 0.0F};
    const vec3f_t unit = {1.0F, 1.0F, 1.0F};
    const quatf_t none = quatf_identity();
    const quatf_t turn = quatf_from_axis_angle(TRS_AXIS, TRS_ANGLE_DEGREES * MATH_RADIANS_PER_DEGREE);
    const mat4f_t local = mat4f_from_trs(origin, none, unit);
    const mat4f_t scaled = mat4f_from_trs(origin, none, TRS_SCALE);
    const mat4f_t rotated = mat4f_from_trs(origin, turn, TRS_SCALE);
    const mat4f_t placed = mat4f_from_trs(TRS_POSITION, turn, TRS_SCALE);

    const transformf_t pose = TRANSFORMF_IDENTITY;
    const render_view_t view = render_view_make(&pose, VIEW_HALF_FOV_SHORT_TAN, VIEW_NEAR_Z, viewport);
    const vec2f_t offset = projectf_offset(&view.lens, VIEW_POINT);
    const vec2f_t screen = projectf_screen(&view.lens, VIEW_POINT);
    int pixel_x = 0;
    int pixel_y = 0;
    r3d_project_screen(VIEW_POINT, &view.lens, &pixel_x, &pixel_y);

    printf("{\n \"trs\": {\n");
    printf("  \"scale\": [%.6g, %.6g], \"angle_degrees\": %.6g, \"position\": [%.6g, %.6g],\n", (double)TRS_SCALE.x,
           (double)TRS_SCALE.y, (double)TRS_ANGLE_DEGREES, (double)TRS_POSITION.x, (double)TRS_POSITION.y);
    print_shape("local", &local, ",");
    print_shape("scaled", &scaled, ",");
    print_shape("rotated", &rotated, ",");
    print_shape("placed", &placed, "");
    printf(" },\n \"pixel\": {\n");
    printf("  \"viewport\": [%d, %d], \"fx\": %.6g, \"fy\": %.6g, \"cx\": %.6g, \"cy\": %.6g, \"near_z\": %.6g,\n",
           viewport.width, viewport.height, (double)view.lens.fx, (double)view.lens.fy, (double)view.lens.cx,
           (double)view.lens.cy, (double)view.lens.near_z);
    printf("  \"camera\": [%.6g, %.6g, %.6g],\n", (double)VIEW_POINT.x, (double)VIEW_POINT.y, (double)VIEW_POINT.z);
    printf("  \"offset\": [%.6g, %.6g],\n", (double)offset.x, (double)offset.y);
    printf("  \"screen\": [%.6g, %.6g],\n", (double)screen.x, (double)screen.y);
    printf("  \"pixel\": [%d, %d]\n }\n}\n", pixel_x, pixel_y);
    return 0;
}
