#include "render/raster.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "render/code_layout.h"
#include "render/r3d_pipeline.h"
#include "render/upscale.h"
#include "util/runtime/frame_cost.h"
#include "util/runtime/job.h"

#define JOB_WAIT_MS 1000

typedef struct {
    const raster_t* raster;
    const r3d_lit_mesh_t* mesh;
    const r3d_lens_t* lens;
    const uint16_t* visible; /* the clusters culling kept */
    int visible_count;
    int first, count; /* clusters of visible[], or rows */
    bool clear;       /* the first mesh of a picture clears it; later ones draw over it */
} slice_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(s->raster);
    r3d_pipeline_transform(s->mesh, s->lens, s->visible + s->first, s->count, b.cs, b.rows);
}

/* A new picture starts from the clear colour, unless upscaling supplies it, and from no depth. */
static void
clear_rows(const raster_t* r, uint16_t* color, uint16_t* depth, size_t pixels) {
    if (!r->upscaled) {
        for (size_t i = 0; i < pixels; i++) {
            color[i] = r->clear;
        }
    }
    memset(depth, 0, pixels * sizeof(*depth));
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

    if (s->clear) {
        clear_rows(r, color, depth, pixels);
    }

    const r3d_span_target_t target = {color, depth, r->width, s->first, s->first + s->count};
    r3d_pipeline_draw(s->mesh, s->lens, s->visible, s->visible_count, b.cs, b.rows, &target);
}

static void
upscale_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(s->raster);
    upscale_rows(&s->raster->upscale, b.color, b.depth, s->raster->clear, s->raster->destination, s->first, s->count);
}

static void
run_split(job_fn_t fn, slice_t first_half, slice_t second_half) {
    (void)job_run_core1(fn, &second_half, sizeof second_half);
    fn(&first_half);
    const bool done = job_wait(JOB_WAIT_MS);
    assert(done); /* the next stage reads what core 1 wrote */
    (void)done;
}

int
raster_vertex_capacity(const raster_t* raster) {
    int most = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        most = raster->instances[i].mesh->vertex_count > most ? raster->instances[i].mesh->vertex_count : most;
    }
    return most;
}

int
raster_cluster_capacity(const raster_t* raster) {
    int most = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        most = raster->instances[i].mesh->cluster_count > most ? raster->instances[i].mesh->cluster_count : most;
    }
    return most;
}

size_t
raster_scratch_bytes(const raster_t* raster) {
    assert(raster->instance_count > 0); /* a raster with nothing to draw would clear nothing */
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    return (sizeof(r3d_pipeline_vertex_t) * (size_t)raster_vertex_capacity(raster))
           + ((sizeof(r3d_pipeline_rows_t) + sizeof(uint16_t)) * (size_t)raster_cluster_capacity(raster))
           + (2 * sizeof(uint16_t) * pixels)
           + (sizeof(uint16_t) * ((size_t)raster->destination_width + (size_t)raster->destination_height));
}

/* The shape the camera frames: the destination's when upscaled, so a render
 * scaled more in one axis than the other still shows the same view. */
static viewport_t
picture_viewport(const raster_t* raster, int quarter) {
    if (raster->upscaled && raster->destination_width > 0 && raster->destination_height > 0) {
        return (viewport_t){raster->destination_width, raster->destination_height, quarter};
    }
    return (viewport_t){raster->width, raster->height, quarter};
}

/* The lens an instance is drawn with: the picture's, fitted to the render
 * size unless `fitted` is false. */
static void
instance_lens(const raster_t* raster, const r3d_instance_t* instance, const camera_t* camera, int quarter, bool fitted,
              r3d_lens_t* lens) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    r3d_lens_init(lens, camera, mesh->position_scale, picture_viewport(raster, quarter));
    if (fitted) {
        r3d_lens_fit(lens, raster->width, raster->height);
    }
    if (instance->placement != NULL) {
        r3d_lens_place(lens, instance->placement, mesh->position_scale);
    }
}

/* Culls one instance into `out`, counting what survived into `stats`. The
 * lens is not fitted, so the frustum is the picture's and the list holds at
 * every size the picture does. */
static int
cull_instance(const raster_t* raster, const r3d_instance_t* instance, const camera_t* camera, int quarter,
              uint16_t* out, raster_stats_t* stats) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    r3d_lens_t lens;
    instance_lens(raster, instance, camera, quarter, false, &lens);
    const int visible = r3d_pipeline_cull(mesh, &lens, out);
    stats->clusters += visible;
    for (int i = 0; i < visible; i++) {
        stats->triangles += mesh->clusters[out[i]].triangle_count;
    }
    return visible;
}

static void
draw_visible(const raster_t* raster, int index, const camera_t* camera, int quarter, const uint16_t* visible,
             int count) {
    const r3d_instance_t* instance = &raster->instances[index];
    const r3d_lit_mesh_t* mesh = instance->mesh;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    const bool clear = index == 0;
    r3d_lens_t lens;
    instance_lens(raster, instance, camera, quarter, true, &lens);

    FRAME_COST_BEGIN(transformed_from);
    const int half = r3d_pipeline_transform_split(mesh, visible, count);
    run_split(transform_slice, (slice_t){raster, mesh, &lens, visible, count, 0, half, clear},
              (slice_t){raster, mesh, &lens, visible, count, half, count - half, clear});
    FRAME_COST_END(transformed_from, "r3d.transform");

    FRAME_COST_BEGIN(drawn_from);
    const int mid = r3d_pipeline_draw_split(mesh, visible, b.rows, count, raster->height);
    run_split(draw_slice, (slice_t){raster, mesh, &lens, visible, count, mid, raster->height - mid, clear},
              (slice_t){raster, mesh, &lens, visible, count, 0, mid, clear});
    FRAME_COST_END(drawn_from, "r3d.draw");
}

RENDER_ENTRY_OFFSET(4) raster_stats_t
raster_draw(const raster_t* raster, const camera_t* camera, int quarter) {
    assert(raster->instance_count > 0);
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    raster_stats_t stats = {0, 0};
    for (int i = 0; i < raster->instance_count; i++) {
        FRAME_COST_BEGIN(culled_from);
        const int visible = cull_instance(raster, &raster->instances[i], camera, quarter, b.visible, &stats);
        FRAME_COST_END(culled_from, "r3d.cull");
        draw_visible(raster, i, camera, quarter, b.visible, visible);
    }
    return stats;
}

size_t
raster_culled_length(const raster_t* raster) {
    size_t length = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        length += 1 + (size_t)raster->instances[i].mesh->cluster_count;
    }
    return length;
}

raster_stats_t
raster_census(const raster_t* raster, const camera_t* camera, int quarter, uint16_t* culled) {
    assert(raster->instance_count > 0);
    raster_stats_t stats = {0, 0};
    FRAME_COST_BEGIN(counted_from);
    size_t at = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        culled[at] = (uint16_t)cull_instance(raster, &raster->instances[i], camera, quarter, culled + at + 1, &stats);
        at += 1 + (size_t)raster->instances[i].mesh->cluster_count;
    }
    FRAME_COST_END(counted_from, "r3d.census");
    return stats;
}

RENDER_ENTRY_OFFSET(4) void
raster_draw_culled(const raster_t* raster, const camera_t* camera, int quarter, const uint16_t* culled) {
    assert(raster->instance_count > 0);
    size_t at = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        draw_visible(raster, i, camera, quarter, culled + at + 1, culled[at]);
        at += 1 + (size_t)raster->instances[i].mesh->cluster_count;
    }
}

RENDER_ENTRY_OFFSET(12) void
raster_upscale(raster_t* raster) {
    assert(raster->upscaled && raster->destination != NULL);
    assert(raster->width > 0 && raster->height > 0);
    assert(raster->destination_width >= raster->width && raster->destination_height >= raster->height);
    FRAME_COST_BEGIN(upscaled_from);
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    uint16_t* columns = b.visible + raster_cluster_capacity(raster);
    uint16_t* rows = columns + raster->destination_width;
    if (raster->upscale.source_width != raster->width || raster->upscale.source_height != raster->height
        || raster->upscale.destination_width != raster->destination_width
        || raster->upscale.destination_height != raster->destination_height || raster->upscale.columns != columns
        || raster->upscale.rows != rows) {
        const bool initialized = upscale_init(&raster->upscale, raster->width, raster->height,
                                              raster->destination_width, raster->destination_height, columns, rows);
        assert(initialized);
    }
    const int mid = raster->destination_height / 2;
    run_split(upscale_slice, (slice_t){raster, NULL, NULL, NULL, 0, mid, raster->destination_height - mid, false},
              (slice_t){raster, NULL, NULL, NULL, 0, 0, mid, false});
    FRAME_COST_END(upscaled_from, "r3d.upscale");
}
