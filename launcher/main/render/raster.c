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
    int visible_count;
    int first, count; /* clusters of visible[], or rows */
    bool clear;       /* the first mesh of a picture clears it; later ones draw over it */
    const r3d_span_writer_t* writers;
    int writer_count;
} slice_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(s->raster);
    r3d_pipeline_transform(s->mesh, s->lens, b.visible + s->first, s->count, b.cs, b.rows);
}

/* A new picture starts from the clear colour, unless upscaling supplies it. */
static void
clear_color(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    gfx_color_t* color = pixels;
    if (!raster->upscaled) {
        for (size_t i = 0; i < count; i++) {
            color[i] = raster->clear;
        }
    }
}

/* And from no depth. */
static void
clear_depth(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

static const raster_attachment_t color_attachment = {sizeof(gfx_color_t), clear_color, NULL, NULL, NULL, NULL, NULL};
static const raster_attachment_t depth_attachment = {sizeof(uint16_t), clear_depth, NULL, NULL, NULL, NULL, NULL};
_Static_assert(R3D_DEPTH_EMPTY == 0, "depth is cleared with memset");

/* Colour, depth, then the caller's, in the order raster_picture() lays them. */
static const raster_attachment_t*
attachment_at(const raster_t* raster, int index) {
    if (index == GFX_ATTACHMENT_COLOR) {
        return &color_attachment;
    }
    return index == GFX_ATTACHMENT_DEPTH ? &depth_attachment : raster->attachments[index - GFX_ATTACHMENT_FURTHER];
}

gfx_render_target_t
raster_picture(const raster_t* raster) {
    assert(raster->attachment_count >= 0 && raster->attachment_count <= GFX_ATTACHMENTS_MAX - GFX_ATTACHMENT_FURTHER);
    gfx_render_target_t picture = {
        raster->width, 0, raster->height, GFX_ATTACHMENT_FURTHER + raster->attachment_count, {{0}}};
    for (int i = 0; i < picture.count; i++) {
        picture.attachment[i].bytes_per_pixel = attachment_at(raster, i)->bytes_per_pixel;
    }
    return picture;
}

void
raster_attachment_tag(const r3d_span_writer_t* writer, const gfx_render_target_t* rows, int y, int x_first, int x_last,
                      int32_t z, int32_t dz) {
    const uint16_t* depth = gfx_render_target_depth(rows, y);
    uint16_t* tag = gfx_render_target_row(rows, writer->attachment, y);
    for (int x = x_first; x <= x_last; x++, z += dz) {
        if ((uint16_t)(z >> 8) == depth[x]) {
            tag[x] = (uint16_t)writer->value;
        }
    }
}

gfx_color_t*
raster_color(const raster_t* raster) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    return gfx_render_target_color(&b.picture, 0);
}

uint16_t*
raster_depth(const raster_t* raster) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    return gfx_render_target_depth(&b.picture, 0);
}

static void
draw_slice(void* ctx) {
    const slice_t* s = ctx;
    const raster_t* r = s->raster;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(r);
    const r3d_span_target_t target = {gfx_render_target_window(&b.picture, s->first, s->first + s->count), s->writers,
                                      s->writer_count};

    if (s->clear) {
        const size_t pixels = (size_t)s->count * (size_t)r->width;
        for (int i = 0; i < target.rows.count; i++) {
            const raster_attachment_t* a = attachment_at(r, i);
            a->clear(a, r, target.rows.attachment[i].pixels, pixels);
        }
    }
    r3d_pipeline_draw(s->mesh, s->lens, b.visible, s->visible_count, b.cs, b.rows, &target);
}

static void
resolve_slice(void* ctx) {
    const slice_t* s = ctx;
    const raster_t* r = s->raster;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(r);
    const gfx_render_target_t rows = gfx_render_target_window(&b.picture, s->first, s->first + s->count);
    for (int i = GFX_ATTACHMENT_FURTHER; i < rows.count; i++) {
        const raster_attachment_t* a = attachment_at(r, i);
        if (a->resolve != NULL) {
            a->resolve(a, r, &rows, i);
        }
    }
}

static void
upscale_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(s->raster);
    upscale_rows(&s->raster->upscale, &b.picture, s->raster->clear, s->raster->destination, s->first, s->count);
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
    const gfx_render_target_t picture = raster_picture(raster);
    return (sizeof(r3d_pipeline_vertex_t) * (size_t)raster_vertex_capacity(raster))
           + ((sizeof(r3d_pipeline_rows_t) + sizeof(uint16_t)) * (size_t)raster_cluster_capacity(raster))
           + gfx_render_target_bytes(&picture)
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

/* Culls one instance into the scratch block's visible list, counting what
 * survived into `stats`, and leaves the lens it culled with in `lens`. */
static int
cull_instance(const raster_t* raster, const r3d_instance_t* instance, const camera_t* camera, int quarter,
              r3d_lens_t* lens, raster_stats_t* stats) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    r3d_lens_init(lens, camera, mesh->position_scale, picture_viewport(raster, quarter));
    r3d_lens_fit(lens, raster->width, raster->height);
    if (instance->placement != NULL) {
        r3d_lens_place(lens, instance->placement, mesh->position_scale);
    }
    const int visible = r3d_pipeline_cull(mesh, lens, b.visible);
    stats->clusters += visible;
    for (int i = 0; i < visible; i++) {
        stats->triangles += mesh->clusters[b.visible[i]].triangle_count;
    }
    return visible;
}

/* The writers of the attachments that write while instance `index` is
 * drawn; returns how many. */
static int
instance_writers(const raster_t* raster, int index, r3d_span_writer_t out[GFX_ATTACHMENTS_MAX]) {
    int count = 0;
    for (int i = 0; i < raster->attachment_count; i++) {
        const raster_attachment_t* a = raster->attachments[i];
        if (a->writer != NULL && a->writer(a, index, &out[count])) {
            out[count++].attachment = GFX_ATTACHMENT_FURTHER + i;
        }
    }
    return count;
}

static void
draw_instance(const raster_t* raster, int index, const camera_t* camera, int quarter, raster_stats_t* stats) {
    const r3d_instance_t* instance = &raster->instances[index];
    const bool clear = index == 0;
    r3d_span_writer_t writers[GFX_ATTACHMENTS_MAX];
    const int writer_count = instance_writers(raster, index, writers);
    const r3d_lit_mesh_t* mesh = instance->mesh;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    r3d_lens_t lens;
    FRAME_COST_BEGIN(culled_from);
    const int visible = cull_instance(raster, instance, camera, quarter, &lens, stats);
    FRAME_COST_END(culled_from, "r3d.cull");

    FRAME_COST_BEGIN(transformed_from);
    const int half = r3d_pipeline_transform_split(mesh, b.visible, visible);
    run_split(transform_slice, (slice_t){raster, mesh, &lens, visible, 0, half, clear, NULL, 0},
              (slice_t){raster, mesh, &lens, visible, half, visible - half, clear, NULL, 0});
    FRAME_COST_END(transformed_from, "r3d.transform");

    FRAME_COST_BEGIN(drawn_from);
    const int mid = r3d_pipeline_draw_split(mesh, b.visible, b.rows, visible, raster->height);
    run_split(draw_slice,
              (slice_t){raster, mesh, &lens, visible, mid, raster->height - mid, clear, writers, writer_count},
              (slice_t){raster, mesh, &lens, visible, 0, mid, clear, writers, writer_count});
    FRAME_COST_END(drawn_from, "r3d.draw");
}

RENDER_ENTRY_OFFSET(4) raster_stats_t
raster_draw(const raster_t* raster, const camera_t* camera, int quarter) {
    assert(raster->instance_count > 0);
    raster_stats_t stats = {0, 0};
    bool resolves = false;
    for (int i = 0; i < raster->attachment_count; i++) {
        const raster_attachment_t* a = raster->attachments[i];
        if (a->begin != NULL) {
            a->begin(a, raster, camera, quarter);
        }
        resolves = resolves || a->resolve != NULL;
    }
    for (int i = 0; i < raster->instance_count; i++) {
        draw_instance(raster, i, camera, quarter, &stats);
    }
    if (resolves) {
        const int mid = raster->height / 2;
        run_split(resolve_slice, (slice_t){raster, NULL, NULL, 0, mid, raster->height - mid, false, NULL, 0},
                  (slice_t){raster, NULL, NULL, 0, 0, mid, false, NULL, 0});
    }
    return stats;
}

raster_stats_t
raster_census(const raster_t* raster, const camera_t* camera, int quarter) {
    assert(raster->instance_count > 0);
    raster_stats_t stats = {0, 0};
    FRAME_COST_BEGIN(counted_from);
    for (int i = 0; i < raster->instance_count; i++) {
        r3d_lens_t lens;
        (void)cull_instance(raster, &raster->instances[i], camera, quarter, &lens, &stats);
    }
    FRAME_COST_END(counted_from, "r3d.census");
    return stats;
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
    run_split(upscale_slice, (slice_t){raster, NULL, NULL, 0, mid, raster->destination_height - mid, false, NULL, 0},
              (slice_t){raster, NULL, NULL, 0, 0, mid, false, NULL, 0});
    FRAME_COST_END(upscaled_from, "r3d.upscale");
}
