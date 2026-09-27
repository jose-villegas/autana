/*
 * GENERATED FILE - do not edit.
 *
 *     python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab \
 *         --name sponza --keep 0.04 --light-tolerance 0.3 --min-edge 60
 *
 * Crytek Sponza (Frank Meinl, Crytek; CC BY 3.0), from the OBJ in
 * McGuire's Computer Graphics Archive, casual-effects.com/data.
 * Decimated, lit by a sun and sky with baked shadows, one sRGB colour
 * per vertex, clusters as the leaves of an octree. Other settings:
 *   --max-edge 900 --sun -0.25 1 0.22
 *   --sun-rays 8 --sky-rays 48 --leaf-triangles 160
 */
#pragma once

#include "render/r3d_lit_mesh.h"

#define SPONZA_VERTEX_COUNT 21398
#define SPONZA_TRIANGLE_COUNT 17381
#define SPONZA_CLUSTER_COUNT 517
#define SPONZA_NODE_COUNT 496
#define SPONZA_POSITION_SCALE 8

extern const r3d_lit_mesh_t sponza_mesh;
