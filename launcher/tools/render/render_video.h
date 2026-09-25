/*
 * render_video - an uncompressed RIFF AVI 1.0 writer for the host render
 * harness: one 'vids' stream of bottom-up BI_RGB 24-bit frames, appended one
 * at a time rather than held in memory. Pure C, host-only - see
 * render_host.c for the frame bytes each call is handed.
 *
 * RIFF AVI 1.0 stores its whole size in a 32-bit field, so callers must
 * check render_video_frames_that_fit()/render_video_total_bytes() before
 * opening a file: this module does not refuse an oversized run itself.
 */
#ifndef RENDER_VIDEO_H
#define RENDER_VIDEO_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE* out;
    int32_t width, height;
    int32_t frame_bytes;
    uint32_t frame_count;
    long riff_size_pos;
    long avih_total_frames_pos;
    long strh_length_pos;
    long movi_size_pos;
    long movi_fourcc_pos;
} render_video_t;

/* One frame's pixel bytes: width's 4-byte-padded BMP row stride times
 * height, the same shape screenshot_bmp_row_stride() defines. int64_t
 * throughout this precheck trio: a large --frames count must be caught
 * before it overflows a 32-bit `long`, which Windows' LLP64 `long` is. */
int64_t render_video_frame_bytes(int32_t width, int32_t height);

/* What one frame adds to the file: its '00dc' chunk header and data, plus
 * its 16-byte idx1 entry. */
int64_t render_video_frame_cost(int32_t width, int32_t height);

/* The file size a `frames`-frame video of `width` x `height` ends at. */
int64_t render_video_total_bytes(int32_t width, int32_t height, int64_t frames);

/* The most frames of `width` x `height` whose video stays within
 * `budget_bytes`; 0 if even one does not fit. */
int64_t render_video_frames_that_fit(int32_t width, int32_t height, int64_t budget_bytes);

/* Opens `path` and writes every header up to the 'movi' list, with
 * frame-count fields left at 0 to be patched by render_video_close().
 * `dt_ms` becomes the stream rate as an exact dwRate/dwScale ratio -
 * 1000/dt_ms reduced by their gcd, not rounded - so it must be nonzero. */
bool render_video_open(render_video_t* v, const char* path, int32_t width, int32_t height, uint32_t dt_ms);

/* Appends one frame: `len` must equal render_video_frame_bytes(v->width,
 * v->height), and `bgr_bottom_up` the same bottom-up 24-bit BGR bytes a BMP
 * body holds. */
bool render_video_write_frame(render_video_t* v, const uint8_t* bgr_bottom_up, int32_t len);

/* Writes the idx1 index and patches every size/count field left open at
 * open(), then closes the file. */
bool render_video_close(render_video_t* v);

#endif
