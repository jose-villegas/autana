#include "draw/draw_image.h"

#include <stdbool.h>
#include <stddef.h>

#include "asset/asset_bytes.h"
#include "draw/draw_image_format.h"

asset_status_t
draw_image_open(asset_view_t entry, draw_image_t* out) {
    *out = (draw_image_t){0};
    if (entry.size < DRAW_IMAGE_HEADER_SIZE) {
        return ASSET_ERR_BOUNDS;
    }
    const uint8_t* at = entry.data;
    if (asset_read_u16(at) != DRAW_IMAGE_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint16_t width = asset_read_u16(at + DRAW_IMAGE_AT_WIDTH);
    const uint16_t height = asset_read_u16(at + DRAW_IMAGE_AT_HEIGHT);
    const uint32_t stride = asset_read_u32(at + DRAW_IMAGE_AT_STRIDE);
    const uint32_t pixels = asset_read_u32(at + DRAW_IMAGE_AT_PIXELS);
    const uint16_t format = asset_read_u16(at + DRAW_IMAGE_AT_FORMAT);
    const bool mono = format == DRAW_IMAGE_MONO1;
    if ((format != DRAW_IMAGE_RGB565 && !mono) || width == 0 || height == 0 || stride < width
        || pixels < DRAW_IMAGE_HEADER_SIZE || (mono && (stride % DRAW_IMAGE_MONO1_STRIDE_STEP) != 0)) {
        return ASSET_ERR_FORMAT;
    }
    const uint64_t last_row = (uint64_t)stride * (height - 1U);
    const uint64_t end =
        mono ? pixels + ((last_row + width + 7U) / 8U) : pixels + ((last_row + width) * sizeof(gfx_color_t));
    if ((!mono && ((uintptr_t)at + pixels) % DRAW_IMAGE_PIXEL_ALIGN != 0) || end > entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    *out = (draw_image_t){
        .format = (draw_image_format_t)format,
        .width = width,
        .height = height,
        .stride = stride,
    };
    if (mono) {
        out->bits = at + pixels;
    } else {
        out->pixels = (const gfx_color_t*)(const void*)(at + pixels);
    }
    return ASSET_OK;
}
