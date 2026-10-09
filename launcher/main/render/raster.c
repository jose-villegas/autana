#include "render/raster.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "render/code_layout.h"
#include "render/r3d_pipeline.h"
#include "render/upscale.h"
#include "util/runtime/frame_cost.h"
#include "util/runtime/job.h"
#include "util/runtime/tune.h"

TUNE_OWNER(render);
TUNE(render, cull, 1, 0, 1);

#define JOB_WAIT_MS 1000

typedef struct {
    const raster_t* raster;
    const r3d_lit_mesh_t* mesh;
    const r3d_lens_t* lens;
    const uint16_t* visible; /* the clusters culling kept */
    int visible_count;
    int first, count; /* clusters of visible[], or rows */
    bool clear;       /* the first mesh of a picture clears it; later ones draw over it */
    const r3d_span_writer_t* writers;
    int writer_count;
    uint16_t* destination;
    int work_index;
    const r3d_pipeline_buffers_t* buffers;
    r3d_span_target_t* target;
} slice_t;

typedef struct {
    r3d_lens_t lens;
    r3d_span_writer_t writers[GFX_ATTACHMENTS_MAX];
    slice_t mine, other;
    r3d_pipeline_buffers_t buffers;
    r3d_span_target_t targets[2];
} draw_work_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");
_Static_assert(_Alignof(draw_work_t) <= R3D_PIPELINE_WORK_ALIGNMENT, "draw work alignment");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(s->raster);
    r3d_pipeline_transform(s->mesh, s->lens, s->visible + s->first, s->count, b.cs, b.rows);
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
    const r3d_pipeline_buffers_t* b = s->buffers;
    r3d_span_target_t* target = s->target;
    *target = (r3d_span_target_t){gfx_render_target_window(&b->picture, s->first, s->first + s->count), s->writers,
                                  s->writer_count};

    if (s->clear) {
        const size_t pixels = (size_t)s->count * (size_t)r->width;
        for (int i = 0; i < target->rows.count; i++) {
            const raster_attachment_t* a = attachment_at(r, i);
            if (a->clear != NULL) {
                a->clear(a, r, target->rows.attachment[i].pixels, pixels);
            }
        }
    }
    r3d_pipeline_draw(s->mesh, s->lens, s->visible, s->visible_count, b->cs, b->rows, target, b->work[s->work_index]);
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
    upscale_rows(&s->raster->upscale, &b.picture, s->raster->clear, s->destination, s->first, s->count);
}

/* Core 1 takes a copy of `second_half`, so a caller may reuse both. */
static void
run_split(job_fn_t fn, slice_t* first_half, const slice_t* second_half) {
    if (!job_try_core1(fn, second_half, sizeof *second_half)) {
        fn((void*)second_half);
    }
    fn(first_half);
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
    const size_t prefix =
        (sizeof(r3d_pipeline_vertex_t) * (size_t)raster_vertex_capacity(raster))
        + (sizeof(r3d_pipeline_rows_t) * (size_t)raster_cluster_capacity(raster)) + r3d_pipeline_culled_bytes(raster)
        + gfx_render_target_bytes(&picture)
        + gfx_attachment_bytes(sizeof(uint16_t), raster->destination_width + raster->destination_height, 1);
    return mathi_size_ceil(prefix, R3D_PIPELINE_WORK_ALIGNMENT) + (2 * r3d_pipeline_work_bytes()) + sizeof(draw_work_t);
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

void
raster_lens(const raster_t* raster, const camera_t* camera, int position_scale, int quarter, r3d_lens_t* lens) {
    r3d_lens_init(lens, camera, position_scale, picture_viewport(raster, quarter));
    r3d_lens_fit(lens, raster->width, raster->height);
}

/* The lens an instance is drawn with: raster_lens(), or the picture's unfitted
 * when `fitted` is false. */
static void
instance_lens(const raster_t* raster, const r3d_instance_t* instance, const camera_t* camera, int quarter, bool fitted,
              r3d_lens_t* lens) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    if (fitted) {
        raster_lens(raster, camera, mesh->position_scale, quarter, lens);
    } else {
        r3d_lens_init(lens, camera, mesh->position_scale, picture_viewport(raster, quarter));
    }
    if (instance->placement != NULL) {
        r3d_lens_place(lens, instance->placement, mesh->position_scale);
    }
}

static int
cull_instance(const r3d_instance_t* instance, const r3d_lens_t* lens, uint16_t* out, raster_stats_t* stats,
              r3d_pipeline_work_t* work) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    int visible;
    if (cull) {
        visible = r3d_pipeline_cull(mesh, lens, out, work);
    } else {
        visible = mesh->cluster_count;
        for (int i = 0; i < visible; i++) {
            out[i] = (uint16_t)i;
        }
    }
    stats->clusters += visible;
    for (int i = 0; i < visible; i++) {
        stats->triangles += mesh->clusters[out[i]].triangle_count;
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
        out[count] = (r3d_span_writer_t){0};
        if (a->writer != NULL && a->writer(a, index, &out[count])) {
            out[count++].attachment = GFX_ATTACHMENT_FURTHER + i;
        }
    }
    return count;
}

static __attribute__((noinline)) draw_work_t*
scratch_draw(const raster_t* raster) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    draw_work_t* work = b.draw;
    work->buffers = b;
    return work;
}

static inline __attribute__((always_inline)) void
draw_visible(const raster_t* raster, int index, const r3d_lens_t* lens, const uint16_t* visible, int count,
             draw_work_t* work) {
    const r3d_instance_t* instance = &raster->instances[index];
    const r3d_lit_mesh_t* mesh = instance->mesh;
    const bool clear = index == 0;
    const int writer_count = instance_writers(raster, index, work->writers);
    FRAME_COST_BEGIN(transformed_from);
    const int half = r3d_pipeline_transform_split(mesh, visible, count);
    slice_t* mine = &work->mine;
    slice_t* other = &work->other;
    *mine = (slice_t){raster, mesh, lens, visible, count, 0, half, clear, NULL, 0, NULL, 0, &work->buffers, NULL};
    *other = *mine;
    other->first = half;
    other->count = count - half;
    run_split(transform_slice, mine, other);
    FRAME_COST_END(transformed_from, "r3d.transform");

    FRAME_COST_BEGIN(drawn_from);
    const int mid = r3d_pipeline_draw_split(mesh, visible, work->buffers.rows, count, raster->height);
    mine->writers = other->writers = work->writers;
    mine->writer_count = other->writer_count = writer_count;
    mine->work_index = 1;
    other->work_index = 0;
    mine->target = &work->targets[1];
    other->target = &work->targets[0];
    mine->first = mid;
    mine->count = raster->height - mid;
    other->first = 0;
    other->count = mid;
    run_split(draw_slice, mine, other);
    FRAME_COST_END(drawn_from, "r3d.draw");
}

/* A NULL stats pointer uses the census list; otherwise each instance is
 * culled. Inlining keeps the draw chain to one entry's stack frame. */
static inline __attribute__((always_inline)) void
draw_instances(const raster_t* raster, const camera_t* camera, int quarter, raster_stats_t* stats) {
    assert(raster->instance_count > 0);
    bool resolves = false;
    for (int i = 0; i < raster->attachment_count; i++) {
        const raster_attachment_t* a = raster->attachments[i];
        if (a->begin != NULL) {
            a->begin(a, raster, camera, quarter);
        }
        resolves = resolves || a->resolve != NULL;
    }
    draw_work_t* work = scratch_draw(raster);
    uint16_t* culled = work->buffers.culled;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t* lens = &work->lens;
        instance_lens(raster, instance, camera, quarter, true, lens);
        if (stats != NULL) {
            FRAME_COST_BEGIN(culled_from);
            culled[0] = (uint16_t)cull_instance(instance, lens, culled + 1, stats, work->buffers.work[0]);
            FRAME_COST_END(culled_from, "r3d.cull");
        }
        draw_visible(raster, i, lens, culled + 1, culled[0], work);
        culled += 1 + (size_t)instance->mesh->cluster_count;
    }
    if (resolves) {
        FRAME_COST_BEGIN(resolved_from);
        const int mid = raster->height / 2;
        work->mine =
            (slice_t){raster, NULL, NULL, NULL, 0, mid, raster->height - mid, false, NULL, 0, NULL, 0, NULL, NULL};
        work->other = (slice_t){raster, NULL, NULL, NULL, 0, 0, mid, false, NULL, 0, NULL, 0, NULL, NULL};
        run_split(resolve_slice, &work->mine, &work->other);
        FRAME_COST_END(resolved_from, "r3d.resolve");
    }
}

RENDER_ENTRY_OFFSET(4) raster_stats_t
raster_draw(const raster_t* raster, const camera_t* camera, int quarter) {
    raster_stats_t stats = {0, 0};
    draw_instances(raster, camera, quarter, &stats);
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
raster_census(const raster_t* raster, const camera_t* camera, int quarter) {
    assert(raster->instance_count > 0);
    raster_stats_t stats = {0, 0};
    FRAME_COST_BEGIN(counted_from);
    draw_work_t* work = scratch_draw(raster);
    uint16_t* culled = work->buffers.culled;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t lens;
        instance_lens(raster, instance, camera, quarter, false, &lens);
        culled[0] = (uint16_t)cull_instance(instance, &lens, culled + 1, &stats, work->buffers.work[0]);
        culled += 1 + (size_t)instance->mesh->cluster_count;
    }
    FRAME_COST_END(counted_from, "r3d.census");
    return stats;
}

RENDER_ENTRY_OFFSET(4) void
raster_draw_culled(const raster_t* raster, const camera_t* camera, int quarter) {
    draw_instances(raster, camera, quarter, NULL);
}

RENDER_ENTRY_OFFSET(12) void
raster_upscale(raster_t* raster, uint16_t* destination, int width, int height) {
    assert(raster->upscaled && destination != NULL);
    assert(raster->width > 0 && raster->height > 0);
    assert(width >= raster->width && height >= raster->height);
    FRAME_COST_BEGIN(upscaled_from);
    uint16_t* columns;
    /* The carve's lifetime ends before the slice pair uses the stack. */
    {
        const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
        const gfx_attachment_t* last = &b.picture.attachment[b.picture.count - 1];
        columns = (uint16_t*)((char*)last->pixels
                              + gfx_attachment_bytes(last->bytes_per_pixel, raster->width, raster->height));
    }
    uint16_t* rows = columns + width;
    if (raster->upscale.source_width != raster->width || raster->upscale.source_height != raster->height
        || raster->upscale.destination_width != width || raster->upscale.destination_height != height
        || raster->upscale.columns != columns || raster->upscale.rows != rows) {
        const bool initialized =
            upscale_init(&raster->upscale, raster->width, raster->height, width, height, columns, rows);
        assert(initialized);
    }
    const int mid = height / 2;
    slice_t mine = {raster, NULL, NULL, NULL, 0, mid, height - mid, false, NULL, 0, destination, 0, NULL, NULL};
    const slice_t other = {raster, NULL, NULL, NULL, 0, 0, mid, false, NULL, 0, destination, 0, NULL, NULL};
    run_split(upscale_slice, &mine, &other);
    FRAME_COST_END(upscaled_from, "r3d.upscale");
}
