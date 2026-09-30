/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \
 *         --name sponza_flat --simplifier meshopt --triangles 17381 --props-share 0.3 --dense-edge 45 --flat
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Simplified, lit by a sun and sky with baked shadows, one sRGB colour
 * per triangle centre. Other settings:
 *   --max-edge 900 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48
 */
#pragma once

#include "render/r3d_lit_mesh.h"

#define SPONZA_FLAT_VERTEX_COUNT 15781
#define SPONZA_FLAT_TRIANGLE_COUNT 17375
#define SPONZA_FLAT_CLUSTER_COUNT 593
#define SPONZA_FLAT_NODE_COUNT 194
#define SPONZA_FLAT_POSITION_SCALE 8

extern const r3d_lit_mesh_t sponza_flat_mesh;
