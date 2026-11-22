#include "render/texture/r3d_texture.h"

#include <stdio.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_entry.h"
#include "core/tune.h"
#include "image/colour/oklab.h"
#include "math/scalar/mathi.h"
#include "render/r3d_span_internal.h"

TUNE_OWNER(texture);
/* 0 opens every texture with no flat level, so every level reads its texels:
 * a figure compares the flat fill against them. Read when a texture opens. */
TUNE(texture, flat_levels, 1, 0, 1);

/* Channel k of palette colour i lit at the tint, times R3D_TEXTURE_ONE: the
 * tint (0..255) times the pattern. */
static int
albedo_x_one(const uint8_t* palette, const uint8_t tint[3], int i, int k) {
    return tint[k] * palette[(i * R3D_TEXTURE_PALETTE_STRIDE) + k];
}

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
        for (int i = 0; i < r3d_texture_colours(t->format); i++) {
            int32_t channel[3];
            for (int k = 0; k < 3; k++) {
                /* albedo * l * 255 / (LEVELS - 1) / tint_luma */
                const int64_t lit = ((int64_t)albedo_x_one(palette, tint, i, k) * l * UINT8_MAX)
                                    / ((int64_t)R3D_TEXTURE_ONE * (R3D_TEXTURE_LIGHT_LEVELS - 1) * tint_luma);
                channel[k] = (int32_t)mathi_min((int)lit, UINT8_MAX) << R3D_SPAN_COLOR_FRAC_BITS;
            }
            t->colormap[l][i] = r3d_span_pack(channel[0], channel[1], channel[2]);
        }
    }
}

/* A bit per palette colour that, lit at the tint, is within
 * R3D_TEXTURE_FLAT_JND of the tint. The flat fill draws the tint, since a
 * pattern lit at R3D_TEXTURE_ONE averages to it. */
static uint32_t
colours_like_the_tint(const uint8_t* palette, const uint8_t tint[3]) {
    const oklabf_t flat = oklabf_from_srgb_bytes(tint[0], tint[1], tint[2]);
    uint32_t like = 0;
    for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
        uint8_t lit[3];
        for (int k = 0; k < 3; k++) {
            lit[k] = (uint8_t)mathi_min(albedo_x_one(palette, tint, i, k) / R3D_TEXTURE_ONE, UINT8_MAX);
        }
        const oklabf_t c = oklabf_from_srgb_bytes(lit[0], lit[1], lit[2]);
        if (oklabf_distance_sq(c, flat) <= R3D_TEXTURE_FLAT_JND * R3D_TEXTURE_FLAT_JND) {
            like |= 1U << i;
        }
    }
    return like;
}

/* A bit per palette colour INDEX4 level `level` of `t` uses. */
static uint32_t
colours_used(const r3d_texture_t* t, int level) {
    const uint32_t all = (1U << R3D_TEXTURE_COLOURS) - 1U;
    const uint32_t bytes = r3d_texture_level_bytes(R3D_TEXTURE_INDEX4, t->size_log2 - level);
    uint32_t used = 0;
    for (uint32_t i = 0; i < bytes && used != all; i++) {
        const uint8_t pair = t->texels[level][i];
        used |= (1U << (pair & R3D_TEXTURE_INDEX4_MASK)) | (1U << (pair >> R3D_TEXTURE_INDEX4_BITS));
    }
    return used;
}

/* The first level of `t` from which every level draws flat unseen: each
 * colour it uses, lit at the tint, like the tint. `levels` when none does,
 * for MASK1, or with texture.flat_levels at 0. */
static uint8_t
flat_level_of(const r3d_texture_t* t, const uint8_t* palette, const uint8_t tint[3]) {
    int level = t->levels;
    if (t->format != R3D_TEXTURE_INDEX4 || flat_levels == 0) {
        return (uint8_t)level;
    }
    const uint32_t like = colours_like_the_tint(palette, tint);
    while (level > 0 && (colours_used(t, level - 1) & ~like) == 0) {
        level--;
    }
    return (uint8_t)level;
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
    const unsigned format = asset_read_u16(data + R3D_TEXTURE_AT_FORMAT);
    const int size_log2 = data[R3D_TEXTURE_AT_SIZE_LOG2];
    const int levels = data[R3D_TEXTURE_AT_LEVELS];
    const int smallest = format == R3D_TEXTURE_MASK1 ? R3D_TEXTURE_MASK1_SIZE_LOG2_MIN : R3D_TEXTURE_SIZE_LOG2_MIN;
    if ((format != R3D_TEXTURE_INDEX4 && format != R3D_TEXTURE_MASK1) || size_log2 < smallest
        || size_log2 > R3D_TEXTURE_SIZE_LOG2_MAX || levels < 1 || size_log2 - (levels - 1) < smallest
        || !asset_entry_zero(*asset, R3D_TEXTURE_AT_TINT + R3D_TEXTURE_TINT_RGB,
                             R3D_TEXTURE_AT_TINT + R3D_TEXTURE_TINT_BYTES)) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t palette_off = asset_read_u32(data + R3D_TEXTURE_AT_PALETTE_OFF);
    const uint32_t texels_off = asset_read_u32(data + R3D_TEXTURE_AT_TEXELS_OFF);
    uint64_t texel_bytes = 0;
    for (int l = 0; l < levels; l++) {
        texel_bytes += r3d_texture_level_bytes(format, size_log2 - l);
    }
    const uint64_t palette_bytes = (uint64_t)r3d_texture_colours(format) * R3D_TEXTURE_PALETTE_STRIDE;
    if (palette_off < R3D_TEXTURE_HEADER_SIZE || texels_off < R3D_TEXTURE_HEADER_SIZE
        || palette_off + palette_bytes > asset->size || texels_off + texel_bytes > asset->size) {
        return ASSET_ERR_BOUNDS;
    }
    uint32_t at = texels_off;
    for (int l = 0; l < levels; l++) {
        texture->texels[l] = data + at;
        at += r3d_texture_level_bytes(format, size_log2 - l);
    }
    texture->format = (uint8_t)format;
    texture->size_log2 = (uint8_t)size_log2;
    texture->levels = (uint8_t)levels;
    texture->flat_level = flat_level_of(texture, data + palette_off, data + R3D_TEXTURE_AT_TINT);
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
        bytes += r3d_texture_level_bytes(texture->format, texture->size_log2 - l);
    }
    return bytes;
}

uint8_t*
r3d_texture_move_texels(r3d_texture_t* texture, uint8_t* to) {
    for (int l = 0; l < texture->levels; l++) {
        const uint32_t bytes = r3d_texture_level_bytes(texture->format, texture->size_log2 - l);
        memcpy(to, texture->texels[l], bytes);
        texture->texels[l] = to;
        to += bytes;
    }
    return to;
}

vec2f_t
r3d_texture_uv(const r3d_mesh_texturing_t* texturing, int cluster, int chart, int vertex) {
    const float unit = 1.0F / (float)(1 << R3D_TEXTURE_UV_SHIFT);
    const int16_t* uv = texturing->uvs[chart == 0 ? vertex : vertex + texturing->cluster_shift[cluster]];
    return (vec2f_t){(float)uv[0] * unit, (float)uv[1] * unit};
}

int
r3d_texture_uvs_material_count(const asset_view_t* uvs) {
    if (asset_entry_validate(*uvs, R3D_TEXTURE_UVS_VERSION, R3D_TEXTURE_UVS_AT_VERSION, R3D_TEXTURE_UVS_HEADER_SIZE,
                             sizeof(int32_t))
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

/* Material `k`'s LTEX sibling of mesh `mesh_id`. */
static asset_status_t
material_view(const asset_pack_t* pack, const char* mesh_id, int k, asset_view_t* view) {
    char suffix[ASSET_NAME_MAX];
    (void)snprintf(suffix, sizeof suffix, "%s%d", R3D_TEXTURE_MATERIAL_SUFFIX, k);
    return r3d_lit_mesh_open_sibling(pack, mesh_id, suffix, R3D_TEXTURE_ASSET, true, view);
}

static asset_status_t
open_materials(const asset_pack_t* pack, const char* mesh_id, int count, r3d_texture_t* materials) {
    for (int k = 0; k < count; k++) {
        asset_view_t view;
        asset_status_t status = material_view(pack, mesh_id, k, &view);
        status = status == ASSET_OK ? r3d_texture_from_asset(&view, &materials[k]) : status;
        if (status != ASSET_OK) {
            return status;
        }
    }
    return ASSET_OK;
}

/* How many charts cluster `c` has, or -1 when one names a material at or
 * past `count` or a NONE chart has one after it. */
static int
cluster_charts(const r3d_mesh_texturing_t* t, int c, int count) {
    int charts = 0;
    while (charts < R3D_TEXTURE_CHARTS && t->cluster_material[c][charts] != R3D_TEXTURE_NONE) {
        if (t->cluster_material[c][charts] >= count) {
            return -1;
        }
        charts++;
    }
    for (int k = charts; k < R3D_TEXTURE_CHARTS; k++) {
        if (t->cluster_material[c][k] != R3D_TEXTURE_NONE) {
            return -1;
        }
    }
    return charts;
}

/* Whether every cluster's charts name materials below `count`, NONE ones
 * last; chart 1's UVs lie inside `uv_count`; and every triangle's code names
 * a chart its cluster has. */
static bool
charts_valid(const r3d_lit_mesh_t* mesh, int count, uint32_t uv_count, const r3d_mesh_texturing_t* t) {
    for (int c = 0; c < mesh->cluster_count; c++) {
        const r3d_lit_cluster_t* cluster = &mesh->clusters[c];
        const int charts = cluster_charts(t, c, count);
        const int64_t shifted = (int64_t)cluster->vertex_first + t->cluster_shift[c];
        if (charts < 0 || (charts > 1 && (shifted < 0 || shifted + cluster->vertex_count > (int64_t)uv_count))) {
            return false;
        }
        for (int i = 0; i < cluster->triangle_count; i++) {
            if (r3d_texture_triangle_code(t, cluster->triangle_first + i) > (unsigned)charts) {
                return false;
            }
        }
    }
    return true;
}

bool
r3d_texture_texturing_fits(const r3d_mesh_texturing_t* texturing, const r3d_lit_mesh_t* mesh) {
    return texturing->vertex_count == (uint32_t)mesh->vertex_count
           && texturing->cluster_count == (uint32_t)mesh->cluster_count
           && texturing->triangle_count == (uint32_t)mesh->triangle_count
           && charts_valid(mesh, texturing->material_count, texturing->uv_count, texturing);
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
    const uint32_t uv_count = asset_read_u32(data + R3D_TEXTURE_UVS_AT_UV_COUNT);
    const uint32_t uvs_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_UVS_OFF);
    const uint32_t shifts_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_SHIFTS_OFF);
    const uint32_t clusters_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_CLUSTERS_OFF);
    const uint32_t triangles_off = asset_read_u32(data + R3D_TEXTURE_UVS_AT_TRIANGLES_OFF);
    if (uv_count < vertices) {
        return ASSET_ERR_FORMAT;
    }
    const uint32_t pair = 2U * sizeof(int16_t);
    const uint32_t codes = (triangles + R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE - 1U) / R3D_TEXTURE_UVS_TRIANGLES_PER_BYTE;
    if (!section_fits(uvs_off, uv_count, pair, sizeof(int16_t), uvs->size)
        || !section_fits(shifts_off, clusters, sizeof(int32_t), sizeof(int32_t), uvs->size)
        || !section_fits(clusters_off, clusters * R3D_TEXTURE_CHARTS, 1U, 1U, uvs->size)
        || !section_fits(triangles_off, codes, 1U, 1U, uvs->size) || triangles_off + codes != uvs->size) {
        return ASSET_ERR_BOUNDS;
    }
    const r3d_mesh_texturing_t read = {.uvs = (const int16_t (*)[2])(const void*)(data + uvs_off),
                                       .cluster_material =
                                           (const uint8_t (*)[R3D_TEXTURE_CHARTS])(const void*)(data + clusters_off),
                                       .cluster_shift = (const int32_t*)(const void*)(data + shifts_off),
                                       .triangle_code = data + triangles_off,
                                       .materials = materials,
                                       .material_count = count,
                                       .vertex_count = vertices,
                                       .cluster_count = clusters,
                                       .triangle_count = triangles,
                                       .uv_count = uv_count};
    if (!r3d_texture_texturing_fits(&read, mesh)) {
        return ASSET_ERR_FORMAT;
    }
    const asset_status_t status = open_materials(pack, mesh_id, count, materials);
    if (status != ASSET_OK) {
        return status;
    }
    *texturing = read;
    return ASSET_OK;
}

asset_status_t
r3d_texture_mesh_cuts(const asset_pack_t* pack, const char* mesh_id, bool* cuts) {
    *cuts = false;
    asset_view_t uvs;
    asset_status_t status =
        r3d_lit_mesh_open_sibling(pack, mesh_id, R3D_TEXTURE_UVS_NAME_SUFFIX, R3D_TEXTURE_UVS_ASSET, false, &uvs);
    if (status != ASSET_OK || uvs.data == NULL) {
        return status;
    }
    const int count = r3d_texture_uvs_material_count(&uvs);
    if (count == 0) {
        return ASSET_ERR_FORMAT;
    }
    bool found = false;
    for (int k = 0; k < count && status == ASSET_OK; k++) {
        asset_view_t view;
        status = material_view(pack, mesh_id, k, &view);
        if (status == ASSET_OK) {
            status =
                asset_entry_validate(view, R3D_TEXTURE_VERSION, R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_HEADER_SIZE, 4U);
        }
        found = found || (status == ASSET_OK && asset_read_u16(view.data + R3D_TEXTURE_AT_FORMAT) == R3D_TEXTURE_MASK1);
    }
    *cuts = status == ASSET_OK && found;
    return status;
}

bool
r3d_texture_texturing_cuts(const r3d_mesh_texturing_t* texturing) {
    for (int k = 0; k < texturing->material_count; k++) {
        if (texturing->materials[k].format == R3D_TEXTURE_MASK1) {
            return true;
        }
    }
    return false;
}
