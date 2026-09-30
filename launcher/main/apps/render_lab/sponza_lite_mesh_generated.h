/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \
 *         --name sponza_lite --simplifier meshopt --triangles 8672 --props-share 0.3 --dense-edge 45
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Simplified, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex, clusters as the leaves of an octree. Other settings:
 *   --max-edge 900 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48 --leaf-triangles 160
 *
 * Rebaked with r3d.rebake: the geometry and colours are the bake's own; the
 * meshlets, cones and octree were rebuilt from them:
 *     python -m r3d.rebake sponza_lite_mesh_generated.c --out-dir . --leaf-triangles 320 --max-depth 10 \
 *         --meshlet-triangles 64 --partition-size 8 --colour-weight 1
 */
#pragma once

#include "render/r3d_lit_mesh.h"

#define SPONZA_LITE_VERTEX_COUNT 7621
#define SPONZA_LITE_TRIANGLE_COUNT 8651
#define SPONZA_LITE_CLUSTER_COUNT 142
#define SPONZA_LITE_NODE_COUNT 89
#define SPONZA_LITE_POSITION_SCALE 8

extern const r3d_lit_mesh_t sponza_lite_mesh;
