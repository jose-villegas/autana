/*
 * r3d_texture: a tiled 4-bit indexed material texture multiplied by a
 * mesh's baked vertex light. The texture holds its material's albedo pattern
 * around its mean, so a vertex colour keeps its meaning, the surface's mean
 * lit colour, and the pattern only modulates it.
 *
 * A texture is an LTEX pack entry (docs/assets/formats/LTEX.md). Textured
 * spans fill with r3d_span's own edge walk, so a textured triangle covers
 * exactly the pixels a plain one would.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "render/r3d_lit_mesh.h"
#include "render/r3d_span.h"

#define R3D_TEXTURE_ASSET          ASSET_TYPE('L', 'T', 'E', 'X')
#define R3D_TEXTURE_VERSION        1U
#define R3D_TEXTURE_INDEX4         1U /* the one format: 16 colours, two texels a byte, low nibble the even column */
#define R3D_TEXTURE_HEADER_SIZE    20U
#define R3D_TEXTURE_AT_VERSION     0U
#define R3D_TEXTURE_AT_FORMAT      2U
#define R3D_TEXTURE_AT_SIZE_LOG2   4U
#define R3D_TEXTURE_AT_LEVELS      5U
#define R3D_TEXTURE_AT_MEAN        6U /* r, g, b: the material's mean albedo, 0..255; then 3 zero bytes */
#define R3D_TEXTURE_MEAN_RGB       3U /* the mean's r, g, b */
#define R3D_TEXTURE_MEAN_BYTES     6U /* the mean and its zero padding */
#define R3D_TEXTURE_AT_PALETTE_OFF 12U
#define R3D_TEXTURE_AT_TEXELS_OFF  16U
#define R3D_TEXTURE_COLOURS        16
#define R3D_TEXTURE_PALETTE_STRIDE 4   /* r, g, b, 0 */
#define R3D_TEXTURE_ONE            128 /* a palette channel of 1.0: the pattern may brighten its mean up to twice */
#define R3D_TEXTURE_SIZE_LOG2_MIN  1   /* the smallest level's side, 2: one byte holds a row */
#define R3D_TEXTURE_SIZE_LOG2_MAX  8   /* 256 texels a side: a span wraps u and v at 8 bits */
#define R3D_TEXTURE_LEVELS_MAX     R3D_TEXTURE_SIZE_LOG2_MAX
/* Light levels of the colormap light: level 0 is black, the last the
 * vertex light whose luminance is 255. */
#define R3D_TEXTURE_LIGHT_LEVELS   32

typedef struct {
    const uint8_t* texels[R3D_TEXTURE_LEVELS_MAX]; /* each level, row-major, half its side's square in bytes */
    uint8_t size_log2;                             /* level 0's side */
    uint8_t levels;                                /* level 0 and its mips, each half the last */
    uint8_t palette[R3D_TEXTURE_COLOURS][R3D_TEXTURE_PALETTE_STRIDE]; /* r, g, b in R3D_TEXTURE_ONE units */
    /* Panel RGB565 of each colour's albedo at each light level. */
    uint16_t colormap[R3D_TEXTURE_LIGHT_LEVELS][R3D_TEXTURE_COLOURS];
} r3d_texture_t;

/* Fills `texture` from `asset`, pointing into it for the texels, so it must
 * outlive the texture, after checking the header and every level against
 * the entry. On failure `texture` is zeroed. */
asset_status_t r3d_texture_from_asset(const asset_view_t* asset, r3d_texture_t* texture);

/* The same for the entry named `id` in `pack`. */
asset_status_t r3d_texture_open(const asset_pack_t* pack, const char* id, r3d_texture_t* texture);

/* Bytes of a level `side_log2` texels a side. */
static inline uint32_t
r3d_texture_level_bytes(int side_log2) {
    return (1U << (2 * side_log2)) / 2U;
}

/* A triangle's corner in texels of level 0. */
typedef struct {
    float u, v;
} r3d_span_uv_t;

/* r3d_span_triangle() with a texture: the same coverage, the same depth
 * test, capped as r3d_span_capped_triangle() when target->skip_hidden. The
 * level is chosen once per triangle from the texels a pixel steps. A target
 * with writers is not supported: its caller draws such a triangle plain. */
void r3d_span_textured_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                                const r3d_span_vertex_t* c, const r3d_span_uv_t uv[3], const r3d_texture_t* texture);

/* A mesh's textures: a material per cluster and a UV per vertex, set on
 * r3d_lit_mesh_t.texturing. A cluster's UVs count from its origin, whole
 * texture sides, so a wall many repeats long still fits int16 at its
 * vertices. */
#define R3D_TEXTURE_NONE     0xFFU /* a cluster drawn with its vertex colours alone */
#define R3D_TEXTURE_UV_SHIFT 4     /* a stored UV is 1/16 texel */

typedef struct r3d_mesh_texturing {
    const int16_t (*uvs)[2];            /* one per vertex, in 1/16 texel of level 0 from its cluster's origin */
    const uint8_t* cluster_material;    /* one per cluster: an index into `materials`, or R3D_TEXTURE_NONE */
    const int16_t (*cluster_origin)[2]; /* one per cluster, in texture sides */
    const r3d_texture_t* materials;
    int material_count;
} r3d_mesh_texturing_t;

/* A mesh's texturing is an LUVS pack entry beside its LMSH
 * (docs/assets/formats/LUVS.md): the UVs, each cluster's origin, then each
 * cluster's material. Material k is the mesh's LTEX sibling
 * <mesh id>R3D_TEXTURE_MATERIAL_SUFFIX<k>. */
#define R3D_TEXTURE_UVS_ASSET             ASSET_TYPE('L', 'U', 'V', 'S')
#define R3D_TEXTURE_UVS_VERSION           1U
#define R3D_TEXTURE_UVS_HEADER_SIZE       24U
#define R3D_TEXTURE_UVS_AT_VERSION        0U
#define R3D_TEXTURE_UVS_AT_MATERIAL_COUNT 2U
#define R3D_TEXTURE_UVS_AT_VERTEX_COUNT   4U
#define R3D_TEXTURE_UVS_AT_CLUSTER_COUNT  8U
#define R3D_TEXTURE_UVS_AT_UVS_OFF        12U
#define R3D_TEXTURE_UVS_AT_ORIGINS_OFF    16U
#define R3D_TEXTURE_UVS_AT_CLUSTERS_OFF   20U /* the cluster materials */
#define R3D_TEXTURE_UVS_NAME_SUFFIX       ".uvs"
#define R3D_TEXTURE_MATERIAL_SUFFIX       ".t"
#define R3D_TEXTURE_MATERIALS_MAX         R3D_TEXTURE_NONE

/* The materials LUVS entry `uvs` names, after checking its header; 0 when
 * the header is malformed. */
int r3d_texture_uvs_material_count(const asset_view_t* uvs);

/* Fills `texturing` from LUVS entry `uvs` of mesh `mesh_id`, `mesh`, after
 * checking it against the mesh, opening each material's LTEX sibling from
 * `pack` into `materials`, `capacity` long. On failure `texturing` is
 * zeroed. */
asset_status_t r3d_texture_texturing_from_asset(const asset_view_t* uvs, const asset_pack_t* pack, const char* mesh_id,
                                                const r3d_lit_mesh_t* mesh, r3d_texture_t* materials, int capacity,
                                                r3d_mesh_texturing_t* texturing);

struct r3d_pipeline_vertex;

/* Draws cluster `cluster` of a textured mesh, its vertices `cs` all in
 * front of the near plane and on screen, as r3d_pipeline_draw() would draw
 * its triangles, but textured. False for a cluster with no material: the
 * caller draws it plain. */
bool r3d_texture_draw_cluster(const r3d_lit_mesh_t* mesh, int cluster, const struct r3d_pipeline_vertex* cs,
                              const r3d_span_target_t* target);
