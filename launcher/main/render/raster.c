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
    const r3d_span_writer_t* writers;
    int writer_count;
} slice_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");

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
    r3d_pipeline_draw(s->mesh, s->lens, s->visible, s->visible_count, b.cs, b.rows, &target);
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

/* Core 1 takes a copy of `second_half`, so a caller may reuse both. */
static void
run_split(job_fn_t fn, slice_t* first_half, const slice_t* second_half) {
    (void)job_run_core1(fn, second_half, sizeof *second_half);
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

/* The scratch block's visible list and cluster rows, out of line so the
 * carve's struct is not on the draw's stack. */
static __attribute__((noinline)) uint16_t*
scratch_visible(const raster_t* raster) {
    return r3d_pipeline_carve(raster).visible;
}

static __attribute__((noinline)) const r3d_pipeline_rows_t*
scratch_rows(const raster_t* raster) {
    return r3d_pipeline_carve(raster).rows;
}

static inline __attribute__((always_inline)) void
draw_visible(const raster_t* raster, int index, const camera_t* camera, int quarter, const uint16_t* visible,
             int count) {
    const r3d_instance_t* instance = &raster->instances[index];
    const r3d_lit_mesh_t* mesh = instance->mesh;
    const bool clear = index == 0;
    r3d_span_writer_t writers[GFX_ATTACHMENTS_MAX];
    const int writer_count = instance_writers(raster, index, writers);
    r3d_lens_t lens;
    instance_lens(raster, instance, camera, quarter, true, &lens);

    FRAME_COST_BEGIN(transformed_from);
    const int half = r3d_pipeline_transform_split(mesh, visible, count);
    slice_t mine = {raster, mesh, &lens, visible, count, 0, half, clear, NULL, 0};
    slice_t other = mine;
    other.first = half;
    other.count = count - half;
    run_split(transform_slice, &mine, &other);
    FRAME_COST_END(transformed_from, "r3d.transform");

    FRAME_COST_BEGIN(drawn_from);
    const int mid = r3d_pipeline_draw_split(mesh, visible, scratch_rows(raster), count, raster->height);
    mine.writers = other.writers = writers;
    mine.writer_count = other.writer_count = writer_count;
    mine.first = mid;
    mine.count = raster->height - mid;
    other.first = 0;
    other.count = mid;
    run_split(draw_slice, &mine, &other);
    FRAME_COST_END(drawn_from, "r3d.draw");
}

/* Draws every instance: from `culled`, raster_census()'s list, or culling
 * each into the scratch block when it is NULL. The attachments begin before
 * the first and resolve after the last. Inlined into both entries, so the
 * draw's stack is no deeper than one entry's frame. */
static inline __attribute__((always_inline)) void
draw_instances(const raster_t* raster, const camera_t* camera, int quarter, const uint16_t* culled,
               raster_stats_t* stats) {
    assert(raster->instance_count > 0);
    bool resolves = false;
    for (int i = 0; i < raster->attachment_count; i++) {
        const raster_attachment_t* a = raster->attachments[i];
        if (a->begin != NULL) {
            a->begin(a, raster, camera, quarter);
        }
        resolves = resolves || a->resolve != NULL;
    }
    uint16_t* const visible = culled == NULL ? scratch_visible(raster) : NULL;
    size_t at = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        const uint16_t* list = visible;
        int count = 0;
        if (culled != NULL) {
            list = culled + at + 1;
            count = culled[at];
            at += 1 + (size_t)raster->instances[i].mesh->cluster_count;
        } else {
            FRAME_COST_BEGIN(culled_from);
            count = cull_instance(raster, &raster->instances[i], camera, quarter, visible, stats);
            FRAME_COST_END(culled_from, "r3d.cull");
        }
        draw_visible(raster, i, camera, quarter, list, count);
    }
    if (resolves) {
        const int mid = raster->height / 2;
        slice_t mine = {raster, NULL, NULL, NULL, 0, mid, raster->height - mid, false, NULL, 0};
        const slice_t other = {raster, NULL, NULL, NULL, 0, 0, mid, false, NULL, 0};
        run_split(resolve_slice, &mine, &other);
    }
}

RENDER_ENTRY_OFFSET(4) raster_stats_t
raster_draw(const raster_t* raster, const camera_t* camera, int quarter) {
    raster_stats_t stats = {0, 0};
    draw_instances(raster, camera, quarter, NULL, &stats);
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
    assert(culled != NULL);
    raster_stats_t unused = {0, 0};
    draw_instances(raster, camera, quarter, culled, &unused);
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
    slice_t mine = {raster, NULL, NULL, NULL, 0, mid, raster->destination_height - mid, false, NULL, 0};
    const slice_t other = {raster, NULL, NULL, NULL, 0, 0, mid, false, NULL, 0};
    run_split(upscale_slice, &mine, &other);
    FRAME_COST_END(upscaled_from, "r3d.upscale");
}
