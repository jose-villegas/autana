/*
 * gfx_render_target: a picture drawn off the panel as a set of attachments,
 * each a per-pixel map of its own format: colour, depth, then any further
 * one a renderer attaches. Every attachment has the target's width and rows,
 * so one row index finds a pixel in all of them. A target may be a window,
 * rows [row0, row1) of a taller picture, and each attachment then starts at
 * row0. Standalone and ESP-IDF-free, like gfx_target.h; what an attachment
 * means beyond its size is its renderer's.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx_color.h"

#define GFX_ATTACHMENT_COLOR   0 /* gfx_color_t, the panel's own format */
#define GFX_ATTACHMENT_DEPTH   1 /* uint16_t, in the renderer's encoding */
#define GFX_ATTACHMENT_FURTHER 2 /* the first of any further ones */
#define GFX_ATTACHMENTS_MAX    4

typedef struct {
    void* pixels; /* the first pixel of row0 */
    int bytes_per_pixel;
} gfx_attachment_t;

typedef struct {
    int width;      /* pixels per row, and every attachment's stride */
    int row0, row1; /* the half-open rows this target holds */
    int count;      /* attachments in use, colour and depth first */
    gfx_attachment_t attachment[GFX_ATTACHMENTS_MAX];
} gfx_render_target_t;

/* A target of colour alone: `width` by `height` pixels at `color`. */
static inline gfx_render_target_t
gfx_render_target_of_color(gfx_color_t* color, int width, int height) {
    return (gfx_render_target_t){width, 0, height, 1, {{color, sizeof(gfx_color_t)}}};
}

/* What one attachment of `rows` rows takes, rounded up to 4 bytes so the
 * next one carved after it starts aligned. */
static inline size_t
gfx_attachment_bytes(int bytes_per_pixel, int width, int rows) {
    return (((size_t)bytes_per_pixel * (size_t)width * (size_t)rows) + 3U) & ~(size_t)3U;
}

/* What every attachment of `target` takes over its rows, carved one after
 * another, from each one's bytes_per_pixel. */
static inline size_t
gfx_render_target_bytes(const gfx_render_target_t* target) {
    size_t bytes = 0;
    for (int i = 0; i < target->count; i++) {
        bytes +=
            gfx_attachment_bytes(target->attachment[i].bytes_per_pixel, target->width, target->row1 - target->row0);
    }
    return bytes;
}

/* Points each attachment at its part of `block`, gfx_render_target_bytes()
 * long and 4-byte aligned; returns the first byte past them. */
static inline char*
gfx_render_target_carve(gfx_render_target_t* target, void* block) {
    char* p = block;
    for (int i = 0; i < target->count; i++) {
        target->attachment[i].pixels = p;
        p += gfx_attachment_bytes(target->attachment[i].bytes_per_pixel, target->width, target->row1 - target->row0);
    }
    return p;
}

/* The same picture's rows [row0, row1), inside the target's own. */
static inline gfx_render_target_t
gfx_render_target_window(const gfx_render_target_t* target, int row0, int row1) {
    gfx_render_target_t window = *target;
    for (int i = 0; i < target->count; i++) {
        const size_t skip =
            (size_t)(row0 - target->row0) * (size_t)target->width * (size_t)target->attachment[i].bytes_per_pixel;
        window.attachment[i].pixels = (char*)target->attachment[i].pixels + skip;
    }
    window.row0 = row0;
    window.row1 = row1;
    return window;
}

/* Attachment `index`'s first pixel of absolute row `y`, inside [row0, row1). */
static inline void*
gfx_render_target_row(const gfx_render_target_t* target, int index, int y) {
    const gfx_attachment_t* a = &target->attachment[index];
    return (char*)a->pixels + ((size_t)(y - target->row0) * (size_t)target->width * (size_t)a->bytes_per_pixel);
}

/* The same for colour and depth, whose sizes are known. */
static inline gfx_color_t*
gfx_render_target_color(const gfx_render_target_t* target, int y) {
    return (gfx_color_t*)target->attachment[GFX_ATTACHMENT_COLOR].pixels + ((y - target->row0) * target->width);
}

static inline uint16_t*
gfx_render_target_depth(const gfx_render_target_t* target, int y) {
    return (uint16_t*)target->attachment[GFX_ATTACHMENT_DEPTH].pixels + ((y - target->row0) * target->width);
}
