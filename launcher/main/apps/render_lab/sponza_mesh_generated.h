/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py <sponza-dir> --out-dir main/apps/render_lab
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Decimated, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex, clusters as the leaves of an octree. Bake settings:
 *   --keep 0.07 --max-edge 200 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48 --leaf-triangles 160
 */
#pragma once

#include "lit_mesh.h"

#define SPONZA_VERTEX_COUNT 53845
#define SPONZA_TRIANGLE_COUNT 43760
#define SPONZA_CLUSTER_COUNT 1120
#define SPONZA_NODE_COUNT 1107
#define SPONZA_POSITION_SCALE 8

extern const lit_mesh_t sponza_mesh;
