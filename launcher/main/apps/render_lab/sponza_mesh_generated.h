/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \
 *         --name sponza --simplifier meshopt --triangles 17381 --props-share 0.3 --dense-edge 45
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Simplified, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex, clusters as the leaves of an octree. Other settings:
 *   --max-edge 900 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48 --leaf-triangles 160
 */
#pragma once

#include "render/r3d_lit_mesh.h"

#define SPONZA_VERTEX_COUNT 19744
#define SPONZA_TRIANGLE_COUNT 17375
#define SPONZA_CLUSTER_COUNT 474
#define SPONZA_NODE_COUNT 470
#define SPONZA_POSITION_SCALE 8

extern const r3d_lit_mesh_t sponza_mesh;
