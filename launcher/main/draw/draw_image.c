#include "draw/draw_image.h"

#include <stdbool.h>
#include <stddef.h>

#include "asset/asset_bytes.h"
#include "draw/draw_image_format.h"
#include "image/pixel/rgb565.h"

asset_status_t
draw_image_open_alpha(asset_view_t entry, image_t* color, image_t* alpha) {
    *color = (image_t){0};
    *alpha = (image_t){0};
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
    const bool mono = format == DRAW_IMAGE_FORMAT_MONO1;
    const bool covered = format == DRAW_IMAGE_FORMAT_RGB565A8;
    if ((format != DRAW_IMAGE_FORMAT_RGB565 && !mono && !covered) || width == 0 || height == 0 || stride < width
        || pixels < DRAW_IMAGE_HEADER_SIZE || (mono && (stride % DRAW_IMAGE_MONO1_STRIDE_STEP) != 0)
        || (covered && (stride % DRAW_IMAGE_RGB565A8_STRIDE_STEP) != 0)) {
        return ASSET_ERR_FORMAT;
    }
    const uint64_t last_row = (uint64_t)stride * (height - 1U);
    /* The A8 plane follows every colour row whole, so its start is fixed by the stride. */
    const uint64_t alpha_at = pixels + ((uint64_t)stride * height * sizeof(rgb565be_t));
    uint64_t end = mono ? pixels + ((last_row + width + 7U) / 8U) : pixels + ((last_row + width) * sizeof(rgb565be_t));
    if (covered) {
        end = alpha_at + last_row + width;
    }
    if ((!mono && ((uintptr_t)at + pixels) % DRAW_IMAGE_PIXEL_ALIGN != 0) || end > entry.size) {
        return ASSET_ERR_BOUNDS;
    }
    const image_format_t kind = mono ? IMAGE_MONO1 : IMAGE_RGB565BE;
    *color = (image_t){.format = kind, .width = width, .height = height, .pitch = IMAGE_PITCH(kind, stride)};
    /* The pack must outlive the image; its pixels are read-only. */
    color->pixels = (void*)(at + pixels);
    if (covered) {
        *alpha = (image_t){.format = IMAGE_A8, .width = width, .height = height, .pitch = stride};
        alpha->pixels = (void*)(at + alpha_at);
    }
    return ASSET_OK;
}

asset_status_t
draw_image_open(asset_view_t entry, image_t* out) {
    image_t alpha;
    const asset_status_t status = draw_image_open_alpha(entry, out, &alpha);
    if (status == ASSET_OK && alpha.pixels != NULL) {
        *out = (image_t){0};
        return ASSET_ERR_FORMAT;
    }
    return status;
}
