/* image: strided pixel views without allocation or copying. */
#pragma once

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "math/geometry/recti.h"

/* Bits per pixel packed into the value, so a byte size is one shift. */
#define IMAGE_FORMAT(id, bits) (((id) << 8) | (bits))

typedef enum {
    IMAGE_RGB565BE = IMAGE_FORMAT(1, 16), /* RGB565 high byte first (rgb565be_t) */
    IMAGE_MONO1 = IMAGE_FORMAT(2, 1),     /* most significant bit leftmost, 1 = ink */
} image_format_t;

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

/* Rows must start on whole bytes. */
static inline void*
image_row(const image_t* image, int y) {
    return (char*)image->pixels + (size_t)y * image->stride * image_format_bits(image->format) / CHAR_BIT;
}

/* The rect must be inside the image; MONO1 x must be a multiple of CHAR_BIT. */
static inline image_t
image_sub(const image_t* image, recti_t rect) {
    assert(image->format != IMAGE_MONO1 || rect.x % CHAR_BIT == 0);
    image_t sub = *image;
    sub.pixels = (char*)image_row(image, rect.y) + (size_t)rect.x * image_format_bits(image->format) / CHAR_BIT;
    sub.width = (uint16_t)rect.w;
    sub.height = (uint16_t)rect.h;
    return sub;
}
