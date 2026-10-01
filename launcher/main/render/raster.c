#include "render/raster.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "render/r3d_pipeline.h"
#include "util/job.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

#define JOB_WAIT_MS 1000

typedef struct {
    const raster_t* raster;
    const r3d_lens_t* lens;
    int visible_count;
    int first, count; /* clusters of visible[], or rows */
} slice_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(s->raster);
    r3d_pipeline_transform(s->raster->mesh, s->lens, b.visible + s->first, s->count, b.cs, b.rows);
}

/* A pixel nothing covered (R3D_DEPTH_EMPTY) takes the clear colour here,
 * so the colour buffer itself is never cleared. */
static void
copy_rows(const raster_t* r, int first, int count) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(r);
    for (int y = first; y < first + count; y++) {
        const size_t row = (size_t)y * (size_t)r->width;
        for (int x = 0; x < r->width; x++) {
            r->destination[row + (size_t)x] =
                b.depth[row + (size_t)x] != R3D_DEPTH_EMPTY ? b.color[row + (size_t)x] : r->clear;
        }
    }
}

/* Two destination rows per source row, each pixel written twice as one
 * 32-bit store; the clear colour fills in as copy_rows() does. */
static void
spread_rows(const raster_t* r, int first, int count) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(r);
    const int out_width = 2 * r->width;
    for (int y = first; y < first + count; y++) {
        const uint16_t* src = b.color + ((size_t)y * (size_t)r->width);
        const uint16_t* depth = b.depth + ((size_t)y * (size_t)r->width);
        uint32_t* top = (uint32_t*)(r->destination + ((size_t)(2 * y) * (size_t)out_width));
        uint32_t* bottom = top + r->width;
        for (int x = 0; x < r->width; x++) {
            const uint16_t c = depth[x] != R3D_DEPTH_EMPTY ? src[x] : r->clear;
            const uint32_t pair = ((uint32_t)c << 16) | c;
            top[x] = pair;
            bottom[x] = pair;
        }
    }
}

static void
draw_slice(void* ctx) {
    const slice_t* s = ctx;
    const raster_t* r = s->raster;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(r);
    const size_t offset = (size_t)s->first * (size_t)r->width;
    const size_t pixels = (size_t)s->count * (size_t)r->width;
    uint16_t* color = b.color + offset;
    uint16_t* depth = b.depth + offset;

    if (r->destination == NULL) {
        for (size_t i = 0; i < pixels; i++) {
            color[i] = r->clear;
        }
    }
    memset(depth, 0, pixels * sizeof(*depth));

    const r3d_span_target_t target = {color, depth, r->width, s->first, s->first + s->count};
    r3d_pipeline_draw(r->mesh, s->lens, b.visible, s->visible_count, b.cs, b.rows, &target);
}

static bool
same_size(const raster_t* r) {
    return r->destination_width == r->width && r->destination_height == r->height;
}

static void
upscale_slice(void* ctx) {
    const slice_t* s = ctx;
    if (same_size(s->raster)) {
        copy_rows(s->raster, s->first, s->count);
    } else {
        spread_rows(s->raster, s->first, s->count);
    }
}

static void
run_split(job_fn_t fn, slice_t first_half, slice_t second_half) {
    (void)job_run_core1(fn, &second_half, sizeof second_half);
    fn(&first_half);
    const bool done = job_wait(JOB_WAIT_MS);
    assert(done); /* the next stage reads what core 1 wrote */
    (void)done;
}

size_t
raster_scratch_bytes(const raster_t* raster) {
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    return (sizeof(r3d_pipeline_vertex_t) * (size_t)raster->mesh->vertex_count)
           + ((sizeof(r3d_pipeline_rows_t) + sizeof(uint16_t)) * (size_t)raster->mesh->cluster_count)
           + (2 * sizeof(uint16_t) * pixels);
}

raster_stats_t
raster_draw(const raster_t* raster, const camera_t* camera, int quarter) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    r3d_lens_t lens;
    r3d_lens_init(&lens, camera, raster->mesh->position_scale, (viewport_t){raster->width, raster->height, quarter});
    const int visible = r3d_pipeline_cull(raster->mesh, &lens, b.visible);
    raster_stats_t stats = {visible, 0};
    for (int i = 0; i < visible; i++) {
        stats.triangles += raster->mesh->clusters[b.visible[i]].triangle_count;
    }

    const int half = r3d_pipeline_transform_split(raster->mesh, b.visible, visible);
    run_split(transform_slice, (slice_t){raster, &lens, visible, 0, half},
              (slice_t){raster, &lens, visible, half, visible - half});

    const int mid = r3d_pipeline_draw_split(raster->mesh, b.visible, b.rows, visible, raster->height);
    run_split(draw_slice, (slice_t){raster, &lens, visible, mid, raster->height - mid},
              (slice_t){raster, &lens, visible, 0, mid});
    return stats;
}

void
raster_upscale(const raster_t* raster) {
    assert(raster->destination != NULL);
    assert(same_size(raster)
           || (raster->destination_width == 2 * raster->width && raster->destination_height == 2 * raster->height));
    const int mid = raster->height / 2;
    run_split(upscale_slice, (slice_t){raster, NULL, 0, mid, raster->height - mid}, (slice_t){raster, NULL, 0, 0, mid});
}
