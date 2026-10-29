/* raster_attachment: per-pixel maps and hooks for raster pictures. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/target/gfx_target.h"
#include "render/render_view.h"

typedef struct raster raster_t;
typedef struct r3d_span_writer r3d_span_writer_t;
typedef struct raster_attachment raster_attachment_t;

struct raster_attachment {
    image_format_t format; /* zero for a view without its own pixels */
    /* Optional. A new picture's `count` pixels, before anything is drawn on them. */
    void (*clear)(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count);
    /* Optional. Once per raster_draw(), before the first instance. */
    void (*begin)(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view);
    /* Optional. Chooses `out->span`, `out->value` and `out->step` for
     * this instance, or returns false to leave this attachment unwritten. */
    bool (*writer)(const raster_attachment_t* self, int instance, r3d_span_writer_t* out);
    /* Optional. Once every instance is drawn, `rows` of the picture, both
     * cores at once on disjoint rows; `index` is this attachment's in it. */
    void (*resolve)(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* rows, int index);
    /* Optional. Development builds: paints `picture`'s colour from this
     * attachment. */
    void (*show)(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index);
    void* state; /* the attachment's own */
};

/* A span writer for a 16- or 32-bit attachment: the pixels this triangle
 * won, those whose depth is the one it filled with, take `writer->value`. */
void raster_attachment_tag(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last,
                           int32_t z, int32_t dz);
