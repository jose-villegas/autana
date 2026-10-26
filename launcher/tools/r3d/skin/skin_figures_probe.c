/*
 * Skinning figure numbers from the library, emitted as JSON for skin_figures.py: a two-joint limb bent about
 * its hinge, skinned by r3d_skin_palette() and r3d_skin_clusters(), and one vertex's way through the inverse
 * bind and the posed joint.
 *
 *   skin_figures_probe INPUT
 *
 * INPUT, written by skin_figures.py: "position_scale hinge_x angles", the angles in degrees, one line; the
 * vertex count; then per vertex "x_ticks y_ticks joint0 joint1 weight0 weight1".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "math/linear/mat4f.h"
#include "math/linear/quatf.h"
#include "math/scalar/math_const.h"
#include "math/text/math_text.h"
#include "render/r3d_skin_format.h"
#include "render/skin/r3d_skinning.h"

enum { JOINTS = 2, INFLUENCES = R3D_SKIN_INFLUENCES_MIN, ANGLES_MAX = 64, TRACED = 0 };

static const vec3f_t BEND_AXIS = {0.0F, 0.0F, 1.0F};
static const vec3f_t UNIT_SCALE = {1.0F, 1.0F, 1.0F};

static mat4f_t
joint_world(int joint, float hinge, float radians) {
    const vec3f_t position = {joint == 0 ? 0.0F : hinge, 0.0F, 0.0F};
    return mat4f_from_trs(position, quatf_from_axis_angle(BEND_AXIS, joint == 0 ? 0.0F : radians), UNIT_SCALE);
}

static void
print_point(vec3f_t p) {
    printf("[%s]", vec2f_text(vec3f_xy(p)).chars);
}

int
main(int argc, char** argv) {
    FILE* in = argc == 2 ? fopen(argv[1], "r") : NULL;
    int scale = 0, angle_count = 0, vertices = 0;
    float hinge = 0.0F, angles[ANGLES_MAX];
    if (in == NULL || fscanf(in, "%d %f %d", &scale, &hinge, &angle_count) != 3 || angle_count < 1
        || angle_count > ANGLES_MAX) {
        fprintf(stderr, "usage: skin_figures_probe INPUT\n");
        return 2;
    }
    for (int a = 0; a < angle_count; a++) {
        if (fscanf(in, "%f", &angles[a]) != 1) {
            return 2;
        }
    }
    if (fscanf(in, "%d", &vertices) != 1 || vertices < 1 || vertices > UINT16_MAX) {
        return 2;
    }
    const int stride = (2 * INFLUENCES) + R3D_SKIN_RECORD_TAIL;
    int16_t (*positions)[3] = calloc((size_t)vertices, sizeof *positions);
    uint8_t* records = calloc((size_t)vertices, (size_t)stride);
    if (positions == NULL || records == NULL) {
        return 1;
    }
    for (int v = 0; v < vertices; v++) {
        int x = 0, y = 0;
        unsigned j0 = 0, j1 = 0, w0 = 0, w1 = 0;
        if (fscanf(in, "%d %d %u %u %u %u", &x, &y, &j0, &j1, &w0, &w1) != 6) {
            return 2;
        }
        positions[v][0] = (int16_t)x;
        positions[v][1] = (int16_t)y;
        uint8_t* r = records + ((size_t)v * (size_t)stride);
        r[0] = (uint8_t)j0;
        r[1] = (uint8_t)j1;
        r[INFLUENCES] = (uint8_t)w0;
        r[INFLUENCES + 1] = (uint8_t)w1;
        r[(2 * INFLUENCES) + 2] = R3D_SKIN_NORMAL_SCALE;
    }
    fclose(in);

    const r3d_lit_cluster_t cluster = {.vertex_first = 0, .vertex_count = (uint16_t)vertices};
    const r3d_lit_node_t node = {.first = 0, .count = 1, .leaf = true};
    const r3d_lit_mesh_t bind = {.positions = (const int16_t (*)[3])positions,
                                 .clusters = &cluster,
                                 .nodes = &node,
                                 .vertex_count = vertices,
                                 .cluster_count = 1,
                                 .node_count = 1,
                                 .position_scale = scale};
    float inverse_binds[JOINTS][R3D_SKIN_MATRIX_WIDTH];
    mat4f_t inverse[JOINTS];
    for (int j = 0; j < JOINTS; j++) {
        inverse[j] = mat4f_invert_affine(joint_world(j, hinge, 0.0F));
        memcpy(inverse_binds[j], inverse[j].m, sizeof inverse_binds[j]);
    }
    const r3d_skin_t skin = {.joint_count = JOINTS,
                             .influences = INFLUENCES,
                             .vertex_count = (uint32_t)vertices,
                             .inverse_binds = (const float (*)[R3D_SKIN_MATRIX_WIDTH])inverse_binds,
                             .vertices = records,
                             .vertex_stride = (uint8_t)stride};
    void* block = malloc(r3d_skin_output_bytes(&bind));
    r3d_skin_output_t out;
    if (block == NULL || !r3d_skin_output_init(&bind, block, r3d_skin_output_bytes(&bind), &out)) {
        return 1;
    }

    /* The traced vertex in model units, bind and joint-local for each of its joints. */
    const float unit = 1.0F / (float)scale;
    const vec3f_t traced = {(float)positions[TRACED][0] * unit, (float)positions[TRACED][1] * unit, 0.0F};
    printf("{\"scale\":%d,\"hinge\":%.7g,\"traced\":{\"bind\":", scale, (double)hinge);
    print_point(traced);
    printf(",\"local\":[");
    for (int j = 0; j < JOINTS; j++) {
        printf(j ? "," : "");
        print_point(mat4f_apply(&inverse[j], traced));
    }
    printf("]},\"poses\":[");
    for (int a = 0; a < angle_count; a++) {
        const float radians = angles[a] * MATH_RADIANS_PER_DEGREE;
        mat4f_t model[JOINTS], palette[JOINTS];
        for (int j = 0; j < JOINTS; j++) {
            model[j] = joint_world(j, hinge, radians);
        }
        r3d_skin_palette(&skin, model, scale, palette);
        r3d_skin_clusters(&skin, &bind, palette, 0, 1, &out);
        printf("%s{\"angle\":%.7g,\"joints\":[", a ? "," : "", (double)angles[a]);
        for (int j = 0; j < JOINTS; j++) {
            printf(j ? "," : "");
            print_point(mat4f_apply(&model[j], (vec3f_t){0}));
        }
        printf("],\"traced\":[");
        for (int j = 0; j < JOINTS; j++) {
            printf(j ? "," : "");
            print_point(mat4f_apply(&model[j], mat4f_apply(&inverse[j], traced)));
        }
        printf("],\"positions\":[");
        for (int v = 0; v < vertices; v++) {
            printf(v ? "," : "");
            print_point((vec3f_t){(float)out.positions[v][0] * unit, (float)out.positions[v][1] * unit, 0.0F});
        }
        printf("]}");
    }
    printf("]}\n");
    free(block);
    free(records);
    free(positions);
    return 0;
}
