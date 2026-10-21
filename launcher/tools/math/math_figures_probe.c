/*
 * Prints JSON for the TRS animation in docs/math/Conventions.md and the
 * camera-to-pixel panels in Projection.md. math_figures.py draws the library results.
 *
 *     math_figures_probe WIDTH HEIGHT
 *
 * WIDTH and HEIGHT set the example camera viewport.
 */
#include <stdio.h>
#include <stdlib.h>

#include "math/linear/mat4f.h"
#include "math/linear/projectf.h"
#include "math/linear/quatf.h"
#include "math/linear/transformf.h"
#include "math/scalar/math_const.h"
#include "render/r3d_project.h"
#include "render/render_view.h"

/* An L-shaped block centred on the origin, so a turn or a mirror shows: local units, the L's
 * corners in drawing order on the back face (z = -SHAPE_DEPTH / 2), then the same corners on the
 * front face (z = SHAPE_DEPTH / 2). */
static const vec3f_t OUTLINE[] = {
    {0.0F, 0.0F, 0.0F}, {1.5F, 0.0F, 0.0F},  {1.5F, 0.6F, 0.0F},
    {0.6F, 0.6F, 0.0F}, {0.6F, 2.25F, 0.0F}, {0.0F, 2.25F, 0.0F},
};
#define OUTLINE_CORNERS ((int)(sizeof OUTLINE / sizeof OUTLINE[0]))
static const float SHAPE_DEPTH = 0.9F;
static const vec3f_t OUTLINE_CENTRE = {0.75F, 1.125F, 0.0F};

#define STEP_FRAMES 16

enum { TRS_SCALE_STEP = 1, TRS_ROTATE_STEP, TRS_TRANSLATE_STEP };

static const vec3f_t TRS_SCALE = {1.6F, 0.6F, 1.4F};
static const vec3f_t TRS_AXIS = {0.0F, 1.0F, 1.0F}; /* normalised below */
static const float TRS_ANGLE_DEGREES = 50.0F;
static const vec3f_t TRS_POSITION = {2.0F, 1.0F, 1.0F};

/* The example camera sits at the origin looking down +z, as TRANSFORMF_IDENTITY places it. */
static const float VIEW_HALF_FOV_SHORT_TAN = 0.5F;
static const float VIEW_NEAR_Z = 0.1F;
static const vec3f_t VIEW_POINT = {0.62F, 0.41F, 2.0F};

static void
print_corners(const mat4f_t* m) {
    printf("[");
    for (int i = 0; i < 2 * OUTLINE_CORNERS; i++) {
        const vec3f_t outline = OUTLINE[i % OUTLINE_CORNERS];
        const vec3f_t corner = {outline.x - OUTLINE_CENTRE.x, outline.y - OUTLINE_CENTRE.y,
                                (i < OUTLINE_CORNERS ? -0.5F : 0.5F) * SHAPE_DEPTH};
        const vec3f_t p = mat4f_apply(m, corner);
        printf("%s[%.6g, %.6g, %.6g]", i ? ", " : "", (double)p.x, (double)p.y, (double)p.z);
    }
    printf("]");
}

static void
print_shape(const char* name, const mat4f_t* m, const char* tail) {
    printf("  \"%s\": ", name);
    print_corners(m);
    printf("%s\n", tail);
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
    const vec3f_t axis = vec3f_normalize(TRS_AXIS);
    const quatf_t turn = quatf_from_axis_angle(axis, TRS_ANGLE_DEGREES * MATH_RADIANS_PER_DEGREE);
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
    printf("  \"scale\": [%.6g, %.6g, %.6g], \"angle_degrees\": %.6g, \"axis\": [%.6g, %.6g, %.6g],\n",
           (double)TRS_SCALE.x, (double)TRS_SCALE.y, (double)TRS_SCALE.z, (double)TRS_ANGLE_DEGREES, (double)axis.x,
           (double)axis.y, (double)axis.z);
    printf("  \"position\": [%.6g, %.6g, %.6g], \"outline_corners\": %d,\n", (double)TRS_POSITION.x,
           (double)TRS_POSITION.y, (double)TRS_POSITION.z, OUTLINE_CORNERS);
    print_shape("local", &local, ",");
    print_shape("scaled", &scaled, ",");
    print_shape("rotated", &rotated, ",");
    print_shape("placed", &placed, ",");
    printf("  \"frames\": [\n");
    for (int step = TRS_SCALE_STEP; step <= TRS_TRANSLATE_STEP; step++) {
        for (int i = 0; i < STEP_FRAMES; i++) {
            const float t = (float)(i + 1) / STEP_FRAMES;
            const vec3f_t scale = step == TRS_SCALE_STEP ? vec3f_lerp(unit, TRS_SCALE, t) : TRS_SCALE;
            const quatf_t rotation = step < TRS_ROTATE_STEP    ? none
                                     : step == TRS_ROTATE_STEP ? quatf_slerp(none, turn, t)
                                                               : turn;
            const vec3f_t position = step == TRS_TRANSLATE_STEP ? vec3f_lerp(origin, TRS_POSITION, t) : origin;
            const mat4f_t matrix = mat4f_from_trs(position, rotation, scale);
            printf("   {\"step\": %d, \"t\": %.6g, \"shape\": ", step, (double)t);
            print_corners(&matrix);
            printf("}%s\n", step == TRS_TRANSLATE_STEP && i == STEP_FRAMES - 1 ? "" : ",");
        }
    }
    printf("  ]\n");
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
