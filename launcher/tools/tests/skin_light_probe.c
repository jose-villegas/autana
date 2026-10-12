/* Native probe of the fused runtime kernel over a flat triangle. */
#include <stdlib.h>
#include <string.h>
#include "render/skin/r3d_skin_light.h"

enum { PATCH_VERTICES = 3, PATCH_CLUSTERS = 1 };

int
probe_patch(const uint8_t (*albedo)[R3D_SKIN_LIGHT_CHANNELS], const float* values, const int8_t* normal, float white,
            uint8_t (*colors)[R3D_SKIN_LIGHT_CHANNELS]) {
    static const int16_t positions[PATCH_VERTICES][3] = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};
    const r3d_lit_cluster_t cluster = {.vertex_count = PATCH_VERTICES};
    const r3d_lit_mesh_t bind = {.positions = positions,
                                 .colors = albedo,
                                 .clusters = &cluster,
                                 .vertex_count = PATCH_VERTICES,
                                 .cluster_count = PATCH_CLUSTERS};

    enum { STRIDE = R3D_SKIN_INFLUENCES_MIN * 2 + R3D_SKIN_RECORD_TAIL };

    uint8_t records[PATCH_VERTICES][STRIDE] = {0};
    for (int v = 0; v < PATCH_VERTICES; v++) {
        records[v][R3D_SKIN_INFLUENCES_MIN] = R3D_SKIN_WEIGHT_SUM;
        memcpy(records[v] + R3D_SKIN_INFLUENCES_MIN * 2, normal, R3D_SKIN_LIGHT_CHANNELS);
    }
    const r3d_skin_t skin = {.joint_count = 1,
                             .influences = R3D_SKIN_INFLUENCES_MIN,
                             .vertex_count = PATCH_VERTICES,
                             .vertices = &records[0][0],
                             .vertex_stride = STRIDE};
    const mat4f_t palette = mat4f_identity();
    const mat3f_t normals = mat3f_from_mat4(palette);
    r3d_skin_light_transfer_t* transfer = malloc(sizeof *transfer);
    const size_t size = r3d_skin_output_bytes(&bind);
    void* storage = malloc(size);
    if (transfer == NULL || storage == NULL) {
        free(transfer);
        free(storage);
        return 0;
    }
    r3d_skin_light_transfer_build(white, transfer);
    const r3d_skin_lights_t lights = {.count = 1,
                                      .direction = {{values[0], values[1], values[2]}},
                                      .color = {{values[3], values[4], values[5]}},
                                      .ambient = {values[6], values[7], values[8]},
                                      .transfer = transfer};
    r3d_skin_output_t output;
    (void)r3d_skin_output_init(&bind, storage, size, &output);
    r3d_skin_clusters_lit(&skin, &bind, &palette, 0, PATCH_CLUSTERS, &output, &normals, &lights, colors);
    free(storage);
    free(transfer);
    return 1;
}
