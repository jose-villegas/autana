/*
 * r3d_mesh_source: a plain indexed mesh with one colour per vertex, and for
 * a textured mesh one UV per vertex, as its caller makes it at run time:
 * read by render/dynamic's build and carried by a frame record
 * (render/record), so a host can build the same mesh again.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/vec3f.h"

typedef struct {
    const vec3f_t* positions;   /* model units */
    const uint8_t (*colors)[3]; /* sRGB, one per vertex */
    int vertex_count;
    /* Wound as the maths winds them: (b - a) x (c - a) points out of the
     * front, right-handed, as iso_surface makes them. The build turns each
     * over for the raster, which in the engine's mirrored frame meets a front
     * face wound the other way about that numeric normal. */
    const uint16_t (*triangles)[3];
    int triangle_count;
    bool double_sided;
    /* One UV per vertex in LUVS's form (1/16 texel, R3D_TEXTURE_UV_SHIFT),
     * which the caller's r3d_mesh_texturing_t reads from the built mesh's
     * `uvs`; NULL for an untextured mesh. Needs a textured capacity. */
    const int16_t (*uvs)[2];
} r3d_mesh_source_t;
