/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py <sponza-dir> --out-dir main/apps/render_lab
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Decimated, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex, clusters as the leaves of an octree, and coarser proxies
 * standing in for some of its nodes. Bake settings:
 *   --keep 0.07 --max-edge 200 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48 --leaf-triangles 160
 *   --view-scale 296.8 --lod-error-px 1 --lod-distance-quantile 0.7 --lod-most 0.7
 *   --lod-colour-tolerance 16 --lod-error-percentile 99
 */
#pragma once

#include "lit_mesh.h"

#define SPONZA_VERTEX_COUNT 61168
#define SPONZA_TRIANGLE_COUNT 51040
#define SPONZA_CLUSTER_COUNT 1160
#define SPONZA_NODE_COUNT 1107
#define SPONZA_POSITION_SCALE 8

extern const lit_mesh_t sponza_mesh;
