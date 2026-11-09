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
    float* depth_near;            /* the census's depth scale, which a draw from its list encodes with: its
                                   * head's first field (raster_census_head_t) */
    uint16_t* culled;             /* raster_culled_length() entries */
    void* draw;                   /* raster-owned instance and slice state */

    union {
        gfx_target_t picture;
        GFX_TARGET_STORAGE(GFX_ATTACHMENTS_MAX) picture_storage;
    }; /* raster_picture(), every row */
} raster_buffers_t;

/* What the census's share of the scratch holds, from one walk of the
 * instances: batch copies and census list entries. */
typedef struct {
    size_t copies;
    size_t culled;
} raster_lengths_t;

raster_lengths_t raster_lengths(const raster_t* raster);

/* The head of the census's share: its depth scale, then where its kept
 * copies are and how many, which the census writes. */
typedef struct {
    float depth_near;
    int copy_count;
    r3d_batch_draw_t* copies;
} raster_census_head_t;

/* Where the census list starts, past the head. */
#define RASTER_CULLED_AT sizeof(raster_census_head_t)

/* Where the kept copies start, past the census list of `lengths`. */
static inline size_t
raster_census_copies_at(raster_lengths_t lengths) {
    return mathi_size_ceil(RASTER_CULLED_AT + (sizeof(uint16_t) * lengths.culled), _Alignof(r3d_batch_draw_t));
}

/* The census's share of the scratch block: its head, raster_culled_length()
 * entries and the copies it keeps, rounded up to whole cache ways. */
static inline size_t
raster_culled_bytes_of(raster_lengths_t lengths) {
    return mathi_size_ceil(raster_census_copies_at(lengths) + (sizeof(r3d_batch_draw_t) * lengths.copies),
                           RASTER_CACHE_WAY);
}

/* raster_culled_bytes_of() for `raster`'s instances and batches. */
size_t raster_culled_bytes(const raster_t* raster);

/* The census's head: the start of `raster`'s scratch block (raster_carve()). */
static inline raster_census_head_t*
raster_census_head(const raster_t* raster) {
    return (raster_census_head_t*)raster->scratch;
}

/* Places the kept copies for `raster`'s instances and batches in its head
 * and returns them. A census does this before filling them. */
static inline r3d_batch_draw_t*
raster_census_place(const raster_t* raster) {
    r3d_batch_draw_t* copies =
        (r3d_batch_draw_t*)((uintptr_t)raster->scratch + raster_census_copies_at(raster_lengths(raster)));
    raster_census_head(raster)->copies = copies;
    return copies;
}

/* The kept copy entry `i` of the drawn list is, or NULL: the list is each
 * of `raster`'s instances, then the copies the last census kept. */
static inline const r3d_batch_draw_t*
raster_census_copy(const raster_t* raster, int i) {
    return i < raster->instance_count ? NULL : &raster_census_head(raster)->copies[i - raster->instance_count];
}

/* Entry `i` of the drawn list: instance `i`, or a kept copy as an instance of its own. */
static inline const r3d_instance_t*
raster_census_entry(const raster_t* raster, int i) {
    return i < raster->instance_count ? &raster->instances[i]
                                      : &raster_census_head(raster)->copies[i - raster->instance_count].instance;
}

/* The drawn list's length: the instances and the copies kept. */
static inline int
raster_census_count(const raster_t* raster) {
    return raster->instance_count + raster_census_head(raster)->copy_count;
}

/* Consecutive instances drawn with one transform and one draw fork/join:
 * as many as fit the buffers sized for the largest mesh, up to this many. */
#define RASTER_BATCH_INSTANCES 16

/* The end of the batch that starts at entry `first` of the last census's
 * drawn list (raster_drawn_at()): past each following
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
    b.culled = (uint16_t*)(p + RASTER_CULLED_AT);
    p += raster_culled_bytes(raster);
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
