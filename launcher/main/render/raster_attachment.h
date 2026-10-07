/*
 * raster_attachment: one per-pixel map a raster draws into, beside or as
 * its colour and depth. The raster carves each from its scratch block at its
 * own size, the drawn width by height, so a new size carves anew and no
 * attachment keeps pixels from one picture to the next. The hooks are how a
 * further attachment takes part in a picture; colour and depth need only
 * `clear`.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/gfx_render_target.h"
#include "render/camera.h"
#include "render/r3d_span.h"

typedef struct raster raster_t;
typedef struct raster_attachment raster_attachment_t;

struct raster_attachment {
    int bytes_per_pixel;
    /* A new picture's `count` pixels, before anything is drawn on them. */
    void (*clear)(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count);
    /* Optional. Once per raster_draw(), before the first instance. */
    void (*begin)(const raster_attachment_t* self, const raster_t* raster, const camera_t* camera, int quarter);
    /* Optional. Fills `out->span` and `out->value` for instance `instance`,
     * or returns false to write nothing while it is drawn. */
    bool (*writer)(const raster_attachment_t* self, int instance, r3d_span_writer_t* out);
    /* Optional. Once every instance is drawn, `rows` of the picture, both
     * cores at once on disjoint rows; `index` is this attachment's in it. */
    void (*resolve)(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* rows,
                    int index);
    /* Optional. Development builds: paints `picture`'s colour from this
     * attachment, for RASTER_SHOW_ATTACHMENT. */
    void (*show)(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* picture,
                 int index);
    void* state; /* the attachment's own */
};

/* A span writer for a 16-bit attachment: the pixels this triangle won,
 * those whose depth is the one it filled with, take `writer->value`. */
void raster_attachment_tag(const r3d_span_writer_t* writer, const gfx_render_target_t* rows, int y, int x_first,
                           int x_last, int32_t z, int32_t dz);
