/*
 * screenshot, the byte-exact pieces of a framebuffer-as-BMP capture: the
 * BMP header, pure arithmetic with no BSP, no
 * USB and no framebuffer, built and checked on a host (see
 * test/suites/suite_screenshot.c).
 *
 * The console listener and the live framebuffer walk are a higher layer,
 * shell/console/console_screenshot.c, since that reaches into gfx and an app's
 * own diagnostic_json().
 */
#pragma once

#include <stdint.h>

/* BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), with no pixel
 * data; see screenshot_bmp_header() below. */
#define SCREENSHOT_BMP_HEADER_SIZE 54

/* Bytes per row once padded to BMP's 4-byte row boundary: width * 3 (24bpp,
 * no alpha), rounded up to the next multiple of 4. */
static inline int32_t
screenshot_bmp_row_stride(int32_t width) {
    return ((width * 3 + 3) / 4) * 4;
}

/* Fills `out[SCREENSHOT_BMP_HEADER_SIZE]` with a BITMAPFILEHEADER +
 * BITMAPINFOHEADER for an uncompressed, bottom-up, 24bpp BMP of `width`
 * x `height` pixels; pixel data follows. Written byte-by-byte in
 * explicit little-endian order, not a packed struct: a compiler may pad
 * struct members for alignment, and BMP's layout has no padding between
 * fields; the two agree only by accident on a particular compiler/ABI.
 * `width`/`height` are taken as given: the one caller always passes
 * GFX_WIDTH/GFX_HEIGHT. */
static inline void
screenshot_bmp_header(uint8_t out[SCREENSHOT_BMP_HEADER_SIZE], int32_t width, int32_t height) {
    const int32_t stride = screenshot_bmp_row_stride(width);
    const uint32_t pixel_bytes = (uint32_t)(stride * height);
    const uint32_t file_size = SCREENSHOT_BMP_HEADER_SIZE + pixel_bytes;

    /* BITMAPFILEHEADER, offsets 0..13 */
    out[0] = 'B';
    out[1] = 'M';
    out[2] = (uint8_t)(file_size);
    out[3] = (uint8_t)(file_size >> 8);
    out[4] = (uint8_t)(file_size >> 16);
    out[5] = (uint8_t)(file_size >> 24);
    out[6] = out[7] = out[8] = out[9] = 0; /* reserved1, reserved2 */
    out[10] = SCREENSHOT_BMP_HEADER_SIZE;  /* bfOffBits: pixels start here */
    out[11] = out[12] = out[13] = 0;

    /* BITMAPINFOHEADER, offsets 14..53 */
    out[14] = 40;
    out[15] = out[16] = out[17] = 0; /* biSize */
    out[18] = (uint8_t)(width);
    out[19] = (uint8_t)(width >> 8);
    out[20] = (uint8_t)(width >> 16);
    out[21] = (uint8_t)(width >> 24);
    out[22] = (uint8_t)(height); /* positive: bottom-up rows */
    out[23] = (uint8_t)(height >> 8);
    out[24] = (uint8_t)(height >> 16);
    out[25] = (uint8_t)(height >> 24);
    out[26] = 1;
    out[27] = 0; /* biPlanes = 1 */
    out[28] = 24;
    out[29] = 0;                               /* biBitCount = 24 */
    out[30] = out[31] = out[32] = out[33] = 0; /* biCompression = BI_RGB */
    out[34] = (uint8_t)(pixel_bytes);
    out[35] = (uint8_t)(pixel_bytes >> 8);
    out[36] = (uint8_t)(pixel_bytes >> 16);
    out[37] = (uint8_t)(pixel_bytes >> 24);
    out[38] = out[39] = out[40] = out[41] = 0; /* biXPelsPerMeter */
    out[42] = out[43] = out[44] = out[45] = 0; /* biYPelsPerMeter */
    out[46] = out[47] = out[48] = out[49] = 0; /* biClrUsed */
    out[50] = out[51] = out[52] = out[53] = 0; /* biClrImportant */
}
