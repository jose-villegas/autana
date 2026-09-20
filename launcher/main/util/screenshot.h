/*
 * screenshot - the byte-exact pieces of a framebuffer-as-BMP capture: the
 * BMP header and the base64 encoding below, pure arithmetic with no BSP, no
 * USB and no framebuffer - built and checked on a host (see
 * test/suites/suite_screenshot.c).
 *
 * The console listener and the live framebuffer walk are a higher layer,
 * console/console_screenshot.c, since that reaches into gfx and an app's
 * own diagnostic_json().
 */
#pragma once

#include <stdint.h>

/*
 * BMP encoding - see console_screenshot.c's write loop for how these two
 * are used together to build one row at a time.
 */

/* BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), with no pixel
 * data - see screenshot_bmp_header() below. */
#define SCREENSHOT_BMP_HEADER_SIZE 54

/* Bytes per row once padded to BMP's 4-byte row boundary: width * 3 (24bpp,
 * no alpha), rounded up to the next multiple of 4. */
static inline int32_t
screenshot_bmp_row_stride(int32_t width) {
    return ((width * 3 + 3) / 4) * 4;
}

/* Fills `out[SCREENSHOT_BMP_HEADER_SIZE]` with a BITMAPFILEHEADER +
 * BITMAPINFOHEADER for an uncompressed, bottom-up, 24bpp BMP of `width`
 * x `height` pixels - pixel data follows. Written byte-by-byte in
 * explicit little-endian order, not a packed struct: a compiler may pad
 * struct members for alignment, and BMP's layout has no padding between
 * fields - the two agree only by accident on a particular compiler/ABI.
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

/*
 * The console UART carries text, so raw pixel bytes cannot go down it
 * unescaped: a stray 0x0A reads as a line break, and many byte values are
 * not valid UTF-8 on their own. Base64 costs a third more over the wire
 * where a hex dump costs two thirds, which matters at 115200 baud for a
 * 322 KiB frame.
 *
 * RFC 4648, no line breaks of its own, '=' padding for a partial group.
 */

/* How many bytes screenshot_base64_encode() writes for `len` input bytes -
 * NOT including a NUL terminator, which callers wanting a C string must
 * budget for separately. */
static inline int32_t
screenshot_base64_encoded_len(int32_t len) {
    return ((len + 2) / 3) * 4;
}

/* Encodes `len` bytes at `in` into `out`, which must hold at least
 * screenshot_base64_encoded_len(len) bytes. Does not NUL-terminate. Pure -
 * no chunking state between calls - which lets a caller call it once per
 * BMP row: every row is a multiple of 3 bytes, so each call ends on a
 * clean group boundary and the '=' padding a partial group needs never
 * occurs for that caller. Tested for a width where it DOES occur
 * (suite_screenshot.c), since a pure function's contract shouldn't depend
 * on its caller. */
static inline void
screenshot_base64_encode(const uint8_t* in, int32_t len, char* out) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    int32_t i = 0, o = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = table[(v >> 6) & 0x3F];
        out[o++] = table[v & 0x3F];
    }

    const int32_t rem = len - i;
    if (rem == 1) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = table[(v >> 18) & 0x3F];
        out[o++] = table[(v >> 12) & 0x3F];
        out[o++] = table[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
}
