/* image: strided pixel views without allocation or copying. */
#pragma once

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "math/geometry/recti.h"

/* Bits per pixel packed into the value, so a byte size is one shift. */
#define IMAGE_FORMAT(id, bits) (((id) << 8) | (bits))

typedef enum {
    IMAGE_RGB565BE = IMAGE_FORMAT(1, 16), /* RGB565 high byte first (rgb565be_t) */
    IMAGE_MONO1 = IMAGE_FORMAT(2, 1),     /* most significant bit leftmost, 1 = ink */
    IMAGE_INDEXED8 = IMAGE_FORMAT(3, 8),  /* one byte per pixel: a palette index */
} image_format_t;

/* A view: its pixels belong to the caller (a pack entry, a buffer) and must outlive it;
 * a view of read-only data must not be written through. */
typedef struct {
    void* pixels;
    image_format_t format;
    uint16_t width, height;
    uint32_t stride; /* pixels from one row's start to the next */
} image_t;

static inline int
image_format_bits(image_format_t format) {
    return format & 0xFF;
}

/* Bytes from one row's start to the next, exact only when stride × bits per
 * pixel is a multiple of CHAR_BIT. A loop over rows takes it once and steps. */
static inline size_t
image_row_bytes(const image_t* image) {
    const int bits = image_format_bits(image->format);
    return bits >= CHAR_BIT ? (size_t)image->stride * (bits / CHAR_BIT) : image->stride / CHAR_BIT;
}

static inline void*
image_row(const image_t* image, int y) {
    return (char*)image->pixels + ((size_t)y * image_row_bytes(image));
}

/* The rect must be inside the image; MONO1 x must be a multiple of CHAR_BIT. */
static inline image_t
image_sub(const image_t* image, recti_t rect) {
    assert(rect.x >= 0 && rect.y >= 0);
    assert(recti_right(rect) <= image->width && recti_bottom(rect) <= image->height);
    assert(image->format != IMAGE_MONO1 || rect.x % CHAR_BIT == 0);
    image_t sub = *image;
    sub.pixels = (char*)image_row(image, rect.y) + (size_t)rect.x * image_format_bits(image->format) / CHAR_BIT;
    sub.width = (uint16_t)rect.w;
    sub.height = (uint16_t)rect.h;
    return sub;
}

/* Most significant bit leftmost, 1 = ink. */
static inline bool
image_mono1_ink(const uint8_t* row, int x) {
    return (row[x / CHAR_BIT] & (uint8_t)((1U << (CHAR_BIT - 1)) >> (x % CHAR_BIT))) != 0;
}
