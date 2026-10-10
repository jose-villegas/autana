#include "gfx/image/gfx_image.h"

#include <stddef.h>

/* The entry's layout, written by tools/gfx/image_asset.py. */
enum {
    HEADER_SIZE = 16,
    AT_FORMAT = 2,
    AT_WIDTH = 4,
    AT_HEIGHT = 6,
    AT_STRIDE = 8,
    AT_PIXELS = 12,
    PIXEL_ALIGN = 4,
};

static uint16_t
half(const uint8_t* at) {
    return (uint16_t)(at[0] | (at[1] << 8));
}

static uint32_t
word(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

asset_status_t
gfx_image_open(asset_view_t entry, gfx_image_t* out) {
    *out = (gfx_image_t){0};
    if (entry.size < HEADER_SIZE) {
        return ASSET_ERR_BOUNDS;
    }
    const uint8_t* at = entry.data;
    if (half(at) != GFX_IMAGE_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint16_t width = half(at + AT_WIDTH);
    const uint16_t height = half(at + AT_HEIGHT);
    const uint32_t stride = word(at + AT_STRIDE);
    const uint32_t pixels = word(at + AT_PIXELS);
    if (half(at + AT_FORMAT) != GFX_IMAGE_RGB565 || width == 0 || height == 0 || stride < width
        || pixels < HEADER_SIZE) {
        return ASSET_ERR_FORMAT;
    }
    const uint64_t end = (uint64_t)pixels + (((uint64_t)stride * (height - 1U)) + width) * sizeof(gfx_color_t);
    if (((uintptr_t)at + pixels) % PIXEL_ALIGN != 0 || end > entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    *out = (gfx_image_t){
        .pixels = (const gfx_color_t*)(const void*)(at + pixels),
        .width = width,
        .height = height,
        .stride = stride,
    };
    return ASSET_OK;
}

asset_status_t
gfx_image_from_pack(const asset_pack_t* pack, const char* id, gfx_image_t* out) {
    asset_view_t entry;
    const asset_status_t status = asset_pack_find(pack, id, GFX_IMAGE_ASSET, &entry);
    if (status != ASSET_OK) {
        *out = (gfx_image_t){0};
        return status;
    }
    return gfx_image_open(entry, out);
}
