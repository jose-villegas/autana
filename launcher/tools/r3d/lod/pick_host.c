/*
 * r3d_lod_pick() over a baked mesh's level arrays and a lens given as its
 * matrix rows, for pick.py through ctypes: the host replays the device's
 * own pick rather than a copy of it.
 */
#include <stdint.h>
#include <string.h>

#include "render/lod/r3d_lod.h"
#include "render/r3d_pipeline.h"

int
pick_host_words(int group_count, int drop_permille) {
    return r3d_lod_words(group_count, drop_permille);
}

/* Updates `coarse` as r3d_lod_pick() does and writes 1 into drawn[c] for
 * each cluster r3d_lod_drawn() keeps, else 0. `rows` is the lens matrix,
 * row-major, 16 floats: position ticks to lens space, as r3d_lens_t.m. */
void
pick_host(const r3d_lit_cluster_lod_t* cluster_lod, int clusters, const r3d_lit_lod_group_t* groups, int group_count,
          const box3s_t* root, int drop_permille, const float* rows, float focal_x, float focal_y, float near_z,
          int height, float tolerance_px, float hysteresis_px, int coverage_permille, uint32_t* coarse,
          uint8_t* drawn) {
    const r3d_lit_node_t node = {.bounds = *root};
    const r3d_lit_mesh_t mesh = {.nodes = &node,
                                 .cluster_count = clusters,
                                 .cluster_lod = cluster_lod,
                                 .lod_groups = groups,
                                 .lod_group_count = group_count};
    r3d_lens_t lens = {.focal_x = focal_x, .focal_y = focal_y, .height = height};
    memcpy(lens.m.m, rows, sizeof lens.m.m);
    lens.project.near_z = near_z;
    /* One pick of one mesh: nothing to spread over frames. */
    const render_lod_settings_t lod = {.tolerance_px = tolerance_px,
                                       .hysteresis_px = hysteresis_px,
                                       .coverage_permille = coverage_permille,
                                       .pick_every = 1};
    r3d_lod_pick(&mesh, &lens, &lod, coarse, NULL);
    r3d_lod_pick_drop(&mesh, &lens, &lod, drop_permille, coarse, NULL);
    const bool dropped = r3d_lod_dropped(&mesh, drop_permille, coarse);
    for (int c = 0; c < clusters; c++) {
        drawn[c] = !dropped && r3d_lod_drawn(&mesh, c, coarse) ? 1U : 0U;
    }
}

/* r3d_lod_group_pixels() of every group, into px[group]. */
void
pick_host_pixels(const r3d_lit_lod_group_t* groups, int group_count, const float* rows, float focal_x, float focal_y,
                 float near_z, float* px) {
    const r3d_lit_mesh_t mesh = {.lod_groups = groups, .lod_group_count = group_count};
    r3d_lens_t lens = {.focal_x = focal_x, .focal_y = focal_y};
    memcpy(lens.m.m, rows, sizeof lens.m.m);
    lens.project.near_z = near_z;
    for (int g = 0; g < group_count; g++) {
        px[g] = r3d_lod_group_pixels(&mesh, &lens, g);
    }
}

float
pick_host_drop_pixels(const box3s_t* root, const float* rows, float focal_x, float focal_y, float near_z) {
    const r3d_lit_node_t node = {.bounds = *root};
    const r3d_lit_mesh_t mesh = {.nodes = &node};
    r3d_lens_t lens = {.focal_x = focal_x, .focal_y = focal_y};
    memcpy(lens.m.m, rows, sizeof lens.m.m);
    lens.project.near_z = near_z;
    return r3d_lod_drop_pixels(&mesh, &lens);
}
