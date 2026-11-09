/*
 * raster_internal: the raster's scratch block as its parts, and the lens it
 * draws through. Internal: render/ and any suite or host tool include it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "math/scalar/mathi.h"
#include "render/r3d_pipeline.h"
#include "render/raster.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

/* One way of the data cache: addresses this far apart share a cache set. */
#if defined(ESP_PLATFORM)
#define RASTER_CACHE_WAY (CONFIG_ESP32S3_DATA_CACHE_SIZE / CONFIG_ESP32S3_DCACHE_ASSOCIATED_WAYS)
#else
#define RASTER_CACHE_WAY 8192 /* the board's 64 KB in 8 ways */
#endif

/* A raster's scratch block as its parts: the one layout, which
 * raster_scratch_bytes() sizes. The census list comes first, so it stays put
 * when the render size changes, and takes whole cache ways, so the parts
 * after it fall in the cache sets they would without it. */
typedef struct {
    r3d_pipeline_vertex_t* cs; /* mesh->vertex_count entries */
    r3d_pipeline_rows_t* rows; /* mesh->cluster_count entries */

    uint16_t* upscale;            /* the upscale maps: destination columns, rows, then row blends */
    r3d_pipeline_work_t* work[2]; /* one block per row slice */
    float* depth_near;            /* the census's depth scale, which a draw from its list encodes with */
    int* drawn_count;             /* entries of `drawn` the census culled */
    const r3d_instance_t** drawn; /* raster_drawn_capacity() entries: each instance, then the copies kept */
    r3d_batch_draw_t* copies;     /* raster_lengths_t.copies entries: the kept copies the drawn list points at */
    uint16_t* culled;             /* raster_culled_length() entries */
    void* draw;                   /* raster-owned instance and slice state */

    union {
        gfx_target_t picture;
        GFX_TARGET_STORAGE(GFX_ATTACHMENTS_MAX) picture_storage;
    }; /* raster_picture(), every row */
} raster_buffers_t;

/* What the census's share of the scratch holds, from one walk of the
 * instances: drawn list entries, batch copies and census list entries. */
typedef struct {
    size_t drawn;
    size_t copies;
    size_t culled;
} raster_lengths_t;

raster_lengths_t raster_lengths(const raster_t* raster);

/* Where the census's drawn list starts, past its depth scale and count. */
#define RASTER_DRAWN_AT mathi_size_ceil(sizeof(float) + sizeof(int), _Alignof(r3d_instance_t*))

/* Where the kept copies start, past the drawn list of `lengths`. */
static inline size_t
raster_copies_at(raster_lengths_t lengths) {
    return mathi_size_ceil(RASTER_DRAWN_AT + (sizeof(r3d_instance_t*) * lengths.drawn), _Alignof(r3d_batch_draw_t));
}

/* Where the census list starts, past the kept copies of `lengths`. */
static inline size_t
raster_culled_at(raster_lengths_t lengths) {
    return raster_copies_at(lengths) + (sizeof(r3d_batch_draw_t) * lengths.copies);
}

/* The census's share of the scratch block: its depth scale, its drawn list,
 * the copies it keeps and raster_culled_length() entries, rounded up to whole
 * cache ways. */
static inline size_t
raster_culled_bytes_of(raster_lengths_t lengths) {
    return mathi_size_ceil(raster_culled_at(lengths) + (sizeof(uint16_t) * lengths.culled), RASTER_CACHE_WAY);
}

static inline size_t
raster_culled_bytes(const raster_t* raster) {
    return raster_culled_bytes_of(raster_lengths(raster));
}

/* The kept copy drawn `entry` of `b` is, or NULL: the copies, and nothing
 * else of the drawn list, lie between b->copies and b->culled. */
static inline const r3d_batch_draw_t*
raster_buffers_copy(const raster_buffers_t* b, const r3d_instance_t* entry) {
    const uintptr_t at = (uintptr_t)entry;
    return at >= (uintptr_t)b->copies && at < (uintptr_t)b->culled ? (const r3d_batch_draw_t*)entry : NULL;
}

/* Consecutive instances drawn with one transform and one draw fork/join:
 * as many as fit the buffers sized for the largest mesh, up to this many. */
#define RASTER_BATCH_INSTANCES 16

/* The end of the batch that starts at entry `first` of the last census's
 * drawn list (raster_drawn()): past each following
 * instance while their vertices and clusters fit those buffers, at most
 * RASTER_BATCH_INSTANCES, and one with render.batched off. */
int raster_batch_end(const raster_t* raster, int first);

/* The lens `raster` draws through: framed on its picture's shape, fitted to
 * the size it renders at, for a mesh of `position_scale`. */
void raster_lens(const raster_t* raster, const render_view_t* view, int position_scale, r3d_lens_t* lens);

/* Carved as integers, so a raster with no block yet still yields the offsets. */
static inline raster_buffers_t
raster_carve(const raster_t* raster) {
    const uintptr_t base = (uintptr_t)raster->scratch;
    uintptr_t p = base;
    raster_buffers_t b;
    b.depth_near = (float*)p;
    b.drawn_count = (int*)(p + sizeof(float));
    const raster_lengths_t lengths = raster_lengths(raster);
    b.drawn = (const r3d_instance_t**)(p + RASTER_DRAWN_AT);
    b.copies = (r3d_batch_draw_t*)(p + raster_copies_at(lengths));
    b.culled = (uint16_t*)(p + raster_culled_at(lengths));
    p += raster_culled_bytes_of(lengths);
    b.cs = (r3d_pipeline_vertex_t*)p;
    p += sizeof(r3d_pipeline_vertex_t) * (size_t)raster_vertex_capacity(raster);
    b.rows = (r3d_pipeline_rows_t*)p;
    p += sizeof(r3d_pipeline_rows_t) * (size_t)raster_cluster_capacity(raster);
    raster_picture(raster, &b.picture);
    p = (uintptr_t)gfx_target_carve(&b.picture, (void*)p);
    b.upscale = (uint16_t*)p;
    p += gfx_attachment_bytes(IMAGE_U16, raster->destination_width + (2 * raster->destination_height), 1);
    p = base + mathi_size_ceil((size_t)(p - base), R3D_PIPELINE_WORK_ALIGNMENT);
    b.work[0] = (r3d_pipeline_work_t*)p;
    b.work[1] = (r3d_pipeline_work_t*)(p + r3d_pipeline_work_bytes());
    b.draw = (void*)(p + (2 * r3d_pipeline_work_bytes()));
    return b;
}
