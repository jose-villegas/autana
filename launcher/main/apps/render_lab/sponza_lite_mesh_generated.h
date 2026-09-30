/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \
 *         --name sponza_lite --simplifier meshopt --triangles 8672 --props-share 0.3 --dense-edge 45
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Simplified, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex. Other settings:
 *   --max-edge 900 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48
 */
#pragma once

#include "render/r3d_lit_mesh.h"

#define SPONZA_LITE_VERTEX_COUNT 8655
#define SPONZA_LITE_TRIANGLE_COUNT 8670
#define SPONZA_LITE_CLUSTER_COUNT 302
#define SPONZA_LITE_NODE_COUNT 105
#define SPONZA_LITE_POSITION_SCALE 8

extern const r3d_lit_mesh_t sponza_lite_mesh;
