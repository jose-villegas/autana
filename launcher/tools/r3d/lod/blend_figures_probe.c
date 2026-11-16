/*
 * Prints JSON for the blend figures in docs/render/frame/Levels-of-Detail.md: one group
 * and its parent's coarse cluster, walked in past the group's switch and back
 * out, through the engine's own pick, morph weight and dither bands at the
 * default settings. blend_figures.py draws the results.
 *
 *     blend_figures_probe
 */
#include <stdint.h>
#include <stdio.h>

#include "image/pixel/bayer.h"
#include "math/text/math_text.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_pipeline.h"
#include "render/settings/render_settings_default.h"

/* The group, in position ticks: a point-sized one, so only its error moves
 * the pick and never the coverage cap. */
#define GROUP_ERROR  100
#define GROUP_RADIUS 1
/* The lens: pixels a unit spans at unit depth, and the picture's height. */
#define FOCAL        1000.0F
#define HEIGHT       240
#define NEAR_Z       0.1F
/* The walk: its steps each way, and the pixels the group's error projects
 * at its far and near ends. */
#define STEPS        240
#define FAR_PX       1.0F
#define NEAR_PX      3.25F

enum { FINE_CLUSTER, COARSE_CLUSTER, CLUSTERS };

static const r3d_lit_lod_group_t GROUPS[1] = {{{{0, 0, 0}, GROUP_RADIUS}, GROUP_ERROR, 0}};
/* The fine cluster belongs to the group; the coarse one was simplified from it. */
static const r3d_lit_cluster_lod_t LODS[CLUSTERS] = {{R3D_LOD_NONE, 0}, {0, R3D_LOD_NONE}};
static const r3d_lit_cluster_t BOXES[CLUSTERS] = {{0}, {0}};
static const int8_t NO_DELTAS[1][3] = {{0, 0, 0}};

/* A lens on the axis, `distance` units back from the group, looking at it. */
static r3d_lens_t
lens_at(float distance) {
    r3d_lens_t lens = {0};
    lens.m = (mat4f_t){{{FOCAL, 0.0F, 0.0F, 0.0F}, {0.0F, FOCAL, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, distance}}};
    lens.focal_x = FOCAL;
    lens.focal_y = FOCAL;
    lens.project = (projectf_t){1.0F, -1.0F, 0.0F, 0.0F, NEAR_Z};
    lens.height = HEIGHT;
    return lens;
}

/* How far back the lens stands for the group's error to project `px`. */
static float
distance_for(float px) {
    return (float)GROUP_ERROR * FOCAL / px;
}

static void
print_step(const r3d_lit_mesh_t* mesh, const render_lod_settings_t* lod, float distance, uint32_t* state, bool first) {
    const r3d_lens_t lens = lens_at(distance);
    uint32_t* arrived = r3d_lod_arrived(state, 1);
    r3d_lod_pick(mesh, &lens, lod, state, true, NULL);
    r3d_lit_mesh_t morphing = *mesh;
    morphing.morph_delta = NO_DELTAS;
    r3d_lod_morph_t morph, dither;
    r3d_lod_morph_init(&morph, &morphing, &lens, lod, state, RENDER_LOD_BLEND_ON);
    r3d_lod_morph_init(&dither, mesh, &lens, lod, state, RENDER_LOD_BLEND_ON);
    printf("%s{\"px\": %s, \"coarse\": %d, \"arrived\": %d, \"blending\": %d, \"weight\": %d, \"fine_band\": %d, "
           "\"coarse_band\": %d}",
           first ? "" : ", ", mathf_text(r3d_lod_group_pixels(mesh, &lens, 0)).chars, bits32_get(state, 0),
           bits32_get(arrived, 0), bits32_get(r3d_lod_blending(arrived, 1), 0),
           r3d_lod_morph_weight(&morph, &morphing, FINE_CLUSTER), r3d_lod_dither_band(&dither, mesh, FINE_CLUSTER),
           r3d_lod_dither_band(&dither, mesh, COARSE_CLUSTER));
}

int
main(void) {
    const render_lod_settings_t lod = RENDER_SETTINGS_DEFAULT.lod;
    const r3d_lit_mesh_t mesh = {.clusters = BOXES,
                                 .cluster_count = CLUSTERS,
                                 .position_scale = 1,
                                 .cluster_lod = LODS,
                                 .lod_groups = GROUPS,
                                 .lod_group_count = 1};
    uint32_t state[R3D_LOD_STATE_WORDS(1)] = {0};
    printf("{\"tolerance_px\": %s, \"hysteresis_px\": %s, \"blend_px\": %s, \"lead\": %s, \"one\": %d, "
           "\"levels\": %d, \"shift\": %d, \"bayer\": [",
           mathf_text(lod.tolerance_px).chars, mathf_text(lod.hysteresis_px).chars, mathf_text(lod.blend_px).chars,
           mathf_text(R3D_LOD_BLEND_LEAD).chars, R3D_LOD_MORPH_ONE, R3D_LOD_DITHER_LEVELS, R3D_LOD_DITHER_SHIFT);
    for (int y = 0; y < BAYER4_SIDE; y++) {
        printf("%s[", y ? ", " : "");
        for (int x = 0; x < BAYER4_SIDE; x++) {
            printf("%s%d", x ? ", " : "", bayer4x4[y][x]);
        }
        printf("]");
    }
    printf("], \"in\": [");
    for (int i = 0; i <= STEPS; i++) {
        print_step(&mesh, &lod, distance_for(FAR_PX + ((NEAR_PX - FAR_PX) * (float)i / (float)STEPS)), state, i == 0);
    }
    printf("], \"out\": [");
    for (int i = 0; i <= STEPS; i++) {
        print_step(&mesh, &lod, distance_for(NEAR_PX + ((FAR_PX - NEAR_PX) * (float)i / (float)STEPS)), state, i == 0);
    }
    printf("]}\n");
    return 0;
}
