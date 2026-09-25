/*
 * render_video - see render_video.h. Every multi-byte field is written
 * byte-by-byte in little-endian order, the same discipline
 * util/screenshot.h's BMP header uses, for the same reason: a compiler may
 * pad a struct, and RIFF's layout has none.
 *
 * idx1's dwOffset is relative to the 'movi' FourCC itself, not the LIST
 * header before it - the convention most AVI 1.0 readers expect - so the
 * first frame's offset is 4, not 0.
 */

#include "render_video.h"

#include <string.h>

#include "util/screenshot.h"

/* RIFF+AVI, LIST hdrl (avih, LIST strl (strh, strf)), LIST movi, idx1
 * header - every byte an AVI carries besides frame data and index entries. */
#define RENDER_VIDEO_FIXED_BYTES (12 + 12 + (8 + 56) + 12 + (8 + 56) + (8 + 40) + 12 + 8)

static void
put_u32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fwrite(b, 1, sizeof(b), f);
}

static void
put_u16(FILE* f, uint16_t v) {
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    fwrite(b, 1, sizeof(b), f);
}

static void
put_fourcc(FILE* f, const char cc[4]) {
    fwrite(cc, 1, 4, f);
}

static void
patch_u32(FILE* f, long pos, uint32_t v) {
    const long here = ftell(f);
    fseek(f, pos, SEEK_SET);
    put_u32(f, v);
    fseek(f, here, SEEK_SET);
}

static uint32_t
gcd_u32(uint32_t a, uint32_t b) {
    while (b != 0) {
        const uint32_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

int64_t
render_video_frame_bytes(int32_t width, int32_t height) {
    return (int64_t)screenshot_bmp_row_stride(width) * height;
}

int64_t
render_video_frame_cost(int32_t width, int32_t height) {
    return 8 + render_video_frame_bytes(width, height) + 16;
}

int64_t
render_video_total_bytes(int32_t width, int32_t height, int64_t frames) {
    return RENDER_VIDEO_FIXED_BYTES + frames * render_video_frame_cost(width, height);
}

int64_t
render_video_frames_that_fit(int32_t width, int32_t height, int64_t budget_bytes) {
    const int64_t usable = budget_bytes - RENDER_VIDEO_FIXED_BYTES;
    if (usable <= 0) {
        return 0;
    }
    return usable / render_video_frame_cost(width, height);
}

bool
render_video_open(render_video_t* v, const char* path, int32_t width, int32_t height, uint32_t dt_ms) {
    memset(v, 0, sizeof(*v));
    if (dt_ms == 0) {
        return false;
    }
    v->out = fopen(path, "wb");
    if (v->out == NULL) {
        return false;
    }
    v->width = width;
    v->height = height;
    v->frame_bytes = (int32_t)render_video_frame_bytes(width, height);

    const uint32_t g = gcd_u32(1000, dt_ms);
    const uint32_t dw_rate = 1000 / g;
    const uint32_t dw_scale = dt_ms / g;

    FILE* f = v->out;
    put_fourcc(f, "RIFF");
    v->riff_size_pos = ftell(f);
    put_u32(f, 0);
    put_fourcc(f, "AVI ");

    put_fourcc(f, "LIST");
    put_u32(f, 4 + (8 + 56) + (8 + (4 + (8 + 56) + (8 + 40)))); /* hdrl: listType + avih + strl LIST */
    put_fourcc(f, "hdrl");

    put_fourcc(f, "avih");
    put_u32(f, 56);
    put_u32(f, dt_ms * 1000u); /* dwMicroSecPerFrame */
    put_u32(f, 0);             /* dwMaxBytesPerSec */
    put_u32(f, 0);             /* dwPaddingGranularity */
    put_u32(f, 0x10);          /* dwFlags = AVIF_HASINDEX */
    v->avih_total_frames_pos = ftell(f);
    put_u32(f, 0); /* dwTotalFrames, patched at close */
    put_u32(f, 0); /* dwInitialFrames */
    put_u32(f, 1); /* dwStreams */
    put_u32(f, (uint32_t)v->frame_bytes);
    put_u32(f, (uint32_t)width);
    put_u32(f, (uint32_t)height);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0);
    put_u32(f, 0); /* dwReserved[4] */

    put_fourcc(f, "LIST");
    put_u32(f, 4 + (8 + 56) + (8 + 40));
    put_fourcc(f, "strl");

    put_fourcc(f, "strh");
    put_u32(f, 56);
    put_fourcc(f, "vids");
    put_fourcc(f, "DIB ");
    put_u32(f, 0); /* dwFlags */
    put_u16(f, 0); /* wPriority */
    put_u16(f, 0); /* wLanguage */
    put_u32(f, 0); /* dwInitialFrames */
    put_u32(f, dw_scale);
    put_u32(f, dw_rate);
    put_u32(f, 0); /* dwStart */
    v->strh_length_pos = ftell(f);
    put_u32(f, 0); /* dwLength, patched at close */
    put_u32(f, (uint32_t)v->frame_bytes);
    put_u32(f, 0xFFFFFFFFu); /* dwQuality: use the codec default */
    put_u32(f, 0);           /* dwSampleSize: samples are not fixed-size */
    put_u16(f, 0);
    put_u16(f, 0); /* rcFrame.left, rcFrame.top */
    put_u16(f, (uint16_t)width);
    put_u16(f, (uint16_t)height); /* rcFrame.right, rcFrame.bottom */

    uint8_t bmp_header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(bmp_header, width, height);
    put_fourcc(f, "strf");
    put_u32(f, 40);
    fwrite(bmp_header + 14, 1, 40, f); /* the BITMAPINFOHEADER half of a BMP header */

    put_fourcc(f, "LIST");
    v->movi_size_pos = ftell(f);
    put_u32(f, 0); /* patched at close */
    v->movi_fourcc_pos = ftell(f);
    put_fourcc(f, "movi");

    return true;
}

bool
render_video_write_frame(render_video_t* v, const uint8_t* bgr_bottom_up, int32_t len) {
    if (len != v->frame_bytes) {
        return false;
    }
    put_fourcc(v->out, "00dc");
    put_u32(v->out, (uint32_t)len);
    if (fwrite(bgr_bottom_up, 1, (size_t)len, v->out) != (size_t)len) {
        return false;
    }
    v->frame_count++;
    return true;
}

bool
render_video_close(render_video_t* v) {
    if (v->out == NULL) {
        return false;
    }
    FILE* f = v->out;

    const uint32_t movi_size = (uint32_t)(ftell(f) - v->movi_fourcc_pos);

    put_fourcc(f, "idx1");
    put_u32(f, v->frame_count * 16u);
    long offset = 4; /* the first '00dc' chunk starts right after 'movi' */
    for (uint32_t i = 0; i < v->frame_count; i++) {
        put_fourcc(f, "00dc");
        put_u32(f, 0x10); /* AVIIF_KEYFRAME: every frame stands on its own */
        put_u32(f, (uint32_t)offset);
        put_u32(f, (uint32_t)v->frame_bytes);
        offset += 8 + v->frame_bytes;
    }

    const uint32_t riff_size = (uint32_t)(ftell(f) - (v->riff_size_pos + 4));
    patch_u32(f, v->riff_size_pos, riff_size);
    patch_u32(f, v->avih_total_frames_pos, v->frame_count);
    patch_u32(f, v->strh_length_pos, v->frame_count);
    patch_u32(f, v->movi_size_pos, movi_size);

    const bool ok = fclose(f) == 0;
    v->out = NULL;
    return ok;
}
