#include "render/texture/r3d_texture.h"

#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "math/scalar/mathi.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"

/* Luminance weights in 1/256 (BT.601), as the light level reads a vertex colour. */
#define LUMA_R     77
#define LUMA_G     150
#define LUMA_B     29
#define LUMA_SHIFT 8

static int
luma(int r, int g, int b) {
    return ((LUMA_R * r) + (LUMA_G * g) + (LUMA_B * b)) >> LUMA_SHIFT;
}

/* Level l lights a colour as a vertex light of luminance l * 255 / (LEVELS - 1)
 * would light the material's mean: its albedo times that over the mean's
 * luminance. */
static void
build_colormap(r3d_texture_t* t, const uint8_t mean[3]) {
    const int mean_luma = mathi_max(luma(mean[0], mean[1], mean[2]), 1);
    for (int l = 0; l < R3D_TEXTURE_LIGHT_LEVELS; l++) {
        for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
            int32_t channel[3];
            for (int k = 0; k < 3; k++) {
                /* albedo (0..255) * light; light = l * 255 / (LEVELS - 1) / mean_luma */
                const int64_t albedo = (int64_t)mean[k] * t->palette[i][k];
                const int64_t lit =
                    (albedo * l * UINT8_MAX) / ((int64_t)R3D_TEXTURE_ONE * (R3D_TEXTURE_LIGHT_LEVELS - 1) * mean_luma);
                channel[k] = (int32_t)mathi_min((int)lit, UINT8_MAX) << 8;
            }
            t->colormap[l][i] = r3d_span_pack(channel[0], channel[1], channel[2]);
        }
    }
}

asset_status_t
r3d_texture_from_asset(const asset_view_t* asset, r3d_texture_t* texture) {
    memset(texture, 0, sizeof *texture);
    asset_status_t status =
        asset_entry_validate(*asset, R3D_TEXTURE_VERSION, R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_HEADER_SIZE, 4U);
    if (status != ASSET_OK) {
        return status;
    }
    const uint8_t* data = asset->data;
    const int size_log2 = data[R3D_TEXTURE_AT_SIZE_LOG2];
    const int levels = data[R3D_TEXTURE_AT_LEVELS];
    if (asset_read_u16(data + R3D_TEXTURE_AT_FORMAT) != R3D_TEXTURE_INDEX4 || size_log2 < R3D_TEXTURE_SIZE_LOG2_MIN
        || size_log2 > R3D_TEXTURE_SIZE_LOG2_MAX || levels < 1 || size_log2 - (levels - 1) < R3D_TEXTURE_SIZE_LOG2_MIN
        || data[R3D_TEXTURE_AT_MEAN + 3] != 0 || asset_read_u16(data + R3D_TEXTURE_AT_MEAN + 4) != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t palette_off = asset_read_u32(data + R3D_TEXTURE_AT_PALETTE_OFF);
    const uint32_t texels_off = asset_read_u32(data + R3D_TEXTURE_AT_TEXELS_OFF);
    uint64_t texel_bytes = 0;
    for (int l = 0; l < levels; l++) {
        texel_bytes += r3d_texture_level_bytes(size_log2 - l);
    }
    const uint64_t palette_bytes = (uint64_t)R3D_TEXTURE_COLOURS * R3D_TEXTURE_PALETTE_STRIDE;
    if (palette_off < R3D_TEXTURE_HEADER_SIZE || texels_off < R3D_TEXTURE_HEADER_SIZE
        || palette_off + palette_bytes > asset->size || texels_off + texel_bytes > asset->size) {
        return ASSET_ERR_BOUNDS;
    }
    memcpy(texture->palette, data + palette_off, (size_t)palette_bytes);
    uint32_t at = texels_off;
    for (int l = 0; l < levels; l++) {
        texture->texels[l] = data + at;
        at += r3d_texture_level_bytes(size_log2 - l);
    }
    texture->size_log2 = (uint8_t)size_log2;
    texture->levels = (uint8_t)levels;
    build_colormap(texture, data + R3D_TEXTURE_AT_MEAN);
    return ASSET_OK;
}

asset_status_t
r3d_texture_open(const asset_pack_t* pack, const char* id, r3d_texture_t* texture) {
    asset_view_t view;
    const asset_status_t status = asset_pack_find(pack, id, R3D_TEXTURE_ASSET, &view);
    if (status != ASSET_OK) {
        memset(texture, 0, sizeof *texture);
        return status;
    }
    return r3d_texture_from_asset(&view, texture);
}

/* A corner of a cluster's triangle: its screen position and colour, and
 * its UV in level-0 texels from the cluster's origin. */
static void
corner(const r3d_lit_mesh_t* mesh, const r3d_pipeline_vertex_t* cs, int v, const float origin[2],
       r3d_span_vertex_t* out, r3d_span_uv_t* uv) {
    const uint8_t* rgb = mesh->colors[v];
    *out = (r3d_span_vertex_t){cs[v].sx, cs[v].sy, cs[v].iz, rgb[0], rgb[1], rgb[2]};
    const float unit = 1.0F / (float)(1 << R3D_TEXTURE_UV_SHIFT);
    *uv = (r3d_span_uv_t){origin[0] + ((float)mesh->texturing->uvs[v][0] * unit),
                          origin[1] + ((float)mesh->texturing->uvs[v][1] * unit)};
}

bool
r3d_texture_draw_cluster(const r3d_lit_mesh_t* mesh, int cluster, const r3d_pipeline_vertex_t* cs,
                         const r3d_span_target_t* target) {
    const r3d_mesh_texturing_t* texturing = mesh->texturing;
    const uint8_t material = texturing->cluster_material[cluster];
    if (material == R3D_TEXTURE_NONE || mesh->colors == NULL) {
        return false;
    }
    const r3d_texture_t* texture = &texturing->materials[material];
    const float side = (float)(1U << texture->size_log2);
    const float origin[2] = {(float)texturing->cluster_origin[cluster][0] * side,
                             (float)texturing->cluster_origin[cluster][1] * side};
    const r3d_lit_cluster_t* c = &mesh->clusters[cluster];
    for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
        r3d_span_vertex_t s[3];
        r3d_span_uv_t uv[3];
        for (int k = 0; k < 3; k++) {
            corner(mesh, cs, mesh->triangles[t][k], origin, &s[k], &uv[k]);
        }
        if (c->double_sided || r3d_span_area2(&s[0], &s[1], &s[2]) < 0) {
            r3d_span_textured_triangle(target, &s[0], &s[1], &s[2], uv, texture);
        }
    }
    return true;
}
