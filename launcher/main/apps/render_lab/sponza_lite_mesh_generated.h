/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py <sponza-dir> --out-dir main/apps/render_lab \
 *         --name sponza_lite --keep 0.025 --light-tolerance 0.4 --min-edge 80
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Decimated, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex, clusters as the leaves of an octree. Other settings:
 *   --max-edge 900 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48 --leaf-triangles 160
 */
#pragma once

#include "lit_mesh.h"

#define SPONZA_LITE_VERTEX_COUNT 10855
#define SPONZA_LITE_TRIANGLE_COUNT 8672
#define SPONZA_LITE_CLUSTER_COUNT 290
#define SPONZA_LITE_NODE_COUNT 274
#define SPONZA_LITE_POSITION_SCALE 8

extern const lit_mesh_t sponza_lite_mesh;
