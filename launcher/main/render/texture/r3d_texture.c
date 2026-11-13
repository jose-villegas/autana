#include "render/texture/r3d_texture.h"

#include <stdio.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "math/scalar/mathi.h"
#include "render/r3d_span_internal.h"

static int
luma(int r, int g, int b) {
    return ((R3D_TEXTURE_LUMA_R * r) + (R3D_TEXTURE_LUMA_G * g) + (R3D_TEXTURE_LUMA_B * b)) >> R3D_TEXTURE_LUMA_SHIFT;
}

/* Level l lights colour i as a vertex colour of the tint's hue and
 * luminance l * 255 / (LEVELS - 1) would: the tint, scaled to that
 * luminance, times the pattern. */
static void
build_colormap(r3d_texture_t* t, const uint8_t* palette, const uint8_t tint[3]) {
    const int tint_luma = mathi_max(luma(tint[0], tint[1], tint[2]), 1);
    for (int l = 0; l < R3D_TEXTURE_LIGHT_LEVELS; l++) {
        for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
            int32_t channel[3];
            for (int k = 0; k < 3; k++) {
                /* tint (0..255) * pattern * l * 255 / (LEVELS - 1) / tint_luma */
                const int64_t albedo = (int64_t)tint[k] * palette[(i * R3D_TEXTURE_PALETTE_STRIDE) + k];
                const int64_t lit =
                    (albedo * l * UINT8_MAX) / ((int64_t)R3D_TEXTURE_ONE * (R3D_TEXTURE_LIGHT_LEVELS - 1) * tint_luma);
                channel[k] = (int32_t)mathi_min((int)lit, UINT8_MAX) << 8;
            }
            t->colormap[l][i] = r3d_span_pack(channel[0], channel[1], channel[2]);
        }
    }
}

static bool
padding_is_zero(const uint8_t* bytes, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
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
        || !padding_is_zero(data + R3D_TEXTURE_AT_TINT + R3D_TEXTURE_TINT_RGB,
                            R3D_TEXTURE_TINT_BYTES - R3D_TEXTURE_TINT_RGB)) {
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
    uint32_t at = texels_off;
    for (int l = 0; l < levels; l++) {
        texture->texels[l] = data + at;
        at += r3d_texture_level_bytes(size_log2 - l);
    }
    texture->size_log2 = (uint8_t)size_log2;
    texture->levels = (uint8_t)levels;
    build_colormap(texture, data + palette_off, data + R3D_TEXTURE_AT_TINT);
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

size_t
r3d_texture_texel_bytes(const r3d_texture_t* texture) {
    size_t bytes = 0;
    for (int l = 0; l < texture->levels; l++) {
        bytes += r3d_texture_level_bytes(texture->size_log2 - l);
    }
    return bytes;
}

uint8_t*
r3d_texture_move_texels(r3d_texture_t* texture, uint8_t* to) {
    for (int l = 0; l < texture->levels; l++) {
        const uint32_t bytes = r3d_texture_level_bytes(texture->size_log2 - l);
        memcpy(to, texture->texels[l], bytes);
        texture->texels[l] = to;
        to += bytes;
    }
    return to;
}

r3d_span_uv_t
r3d_texture_uv(const r3d_mesh_texturing_t* texturing, const r3d_texture_t* texture, int cluster, int vertex) {
    const float side = (float)(1U << texture->size_log2);
    const float unit = 1.0F / (float)(1 << R3D_TEXTURE_UV_SHIFT);
    return (r3d_span_uv_t){
        ((float)texturing->cluster_origin[cluster][0] * side) + ((float)texturing->uvs[vertex][0] * unit),
        ((float)texturing->cluster_origin[cluster][1] * side) + ((float)texturing->uvs[vertex][1] * unit)};
}

int
r3d_texture_uvs_material_count(const asset_view_t* uvs) {
    if (asset_entry_validate(*uvs, R3D_TEXTURE_UVS_VERSION, R3D_TEXTURE_UVS_AT_VERSION, R3D_TEXTURE_UVS_HEADER_SIZE,
                             sizeof(int16_t))
        != ASSET_OK) {
        return 0;
    }
    return asset_read_u16(uvs->data + R3D_TEXTURE_UVS_AT_MATERIAL_COUNT);
}

/* Whether `count` records of `size` bytes at `offset` lie inside an entry of
 * `entry_size` bytes, after its header and aligned to `alignment`. */
static bool
section_fits(uint32_t offset, uint32_t count, uint32_t size, uint32_t alignment, uint32_t entry_size) {
    return offset >= R3D_TEXTURE_UVS_HEADER_SIZE && offset % alignment == 0
           && (uint64_t)offset + ((uint64_t)count * size) <= entry_size;
}

static asset_status_t
open_materials(const asset_pack_t* pack, const char* mesh_id, int count, r3d_texture_t* materials) {
    for (int k = 0; k < count; k++) {
        char suffix[ASSET_NAME_MAX];
        (void)snprintf(suffix, sizeof suffix, "%s%d", R3D_TEXTURE_MATERIAL_SUFFIX, k);
        asset_view_t view;
        asset_status_t status = r3d_lit_mesh_open_sibling(pack, mesh_id, suffix, R3D_TEXTURE_ASSET, true, &view);
        status = status == ASSET_OK ? r3d_texture_from_asset(&view, &materials[k]) : status;
        if (status != ASSET_OK) {
            return status;
        }
    }
    return ASSET_OK;
}

asset_status_t
r3d_texture_texturing_from_asset(const asset_view_t* uvs, const asset_pack_t* pack, const char* mesh_id,
                                 const r3d_lit_mesh_t* mesh, r3d_texture_t* materials, int capacity,
                                 r3d_mesh_texturing_t* texturing) {
    *texturing = (r3d_mesh_texturing_t){0};
    const int count = r3d_texture_uvs_material_count(uvs);
    const uint8_t* data = uvs->data;
    if (count == 0 || count > capacity) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t vertices = asset_read_u32(data + R3D_TEXTURE_UVS_AT_VERTEX_COUNT);
    const uint32_t clusters = asset_read_u32(data + R3D_TEXTURE_UVS_AT_CLUSTER_COUNT);
    const uint32_t triangles = asset_read_u32(data + R3D_TEXTURE_UVS_AT_TRIANGLE_COUNT);
    const uint32_t uvs_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_UVS_OFF);
    const uint32_t origins_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_ORIGINS_OFF);
    const uint32_t clusters_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF);
    const uint32_t triangles_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_TRIANGLES_OFF);
    if (vertices != (uint32_t)mesh->vertex_count || clusters != (uint32_t)mesh->cluster_count
        || triangles != (uint32_t)mesh->triangle_count) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t pair = 2U * sizeof(int16_t);
    const uint32_t bits = (triangles + R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE - 1U) / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE;
    if (!section_fits(uvs_off, vertices, pair, sizeof(int16_t), uvs->size)
        || !section_fits(origins_off, clusters, pair, sizeof(int16_t), uvs->size)
        || !section_fits(clusters_off, clusters, 1U, 1U, uvs->size)
        || !section_fits(triangles_off, bits, 1U, 1U, uvs->size) || triangles_off + bits != uvs->size) {
        return ASSET_ERR_BOUNDS;
    }
    for (uint32_t c = 0; c < clusters; c++) {
        const uint8_t material = data[clusters_off + c];
        if (material != R3D_TEXTURE_NONE && material >= count) {
            return ASSET_ERR_FORMAT;
        }
    }
    const asset_status_t status = open_materials(pack, mesh_id, count, materials);
    if (status != ASSET_OK) {
        return status;
    }
    *texturing = (r3d_mesh_texturing_t){.uvs = (const int16_t (*)[2])(const void*)(data + uvs_off),
                                        .cluster_material = data + clusters_off,
                                        .cluster_origin = (const int16_t (*)[2])(const void*)(data + origins_off),
                                        .triangle_textured = data + triangles_off,
                                        .materials = materials,
                                        .material_count = count};
    return ASSET_OK;
}
