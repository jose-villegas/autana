#include "render/raster.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "core/job.h"
#include "core/timing.h"
#include "profile/frame_cost.h"
#include "render/r3d_pipeline.h"
#include "render/upscale.h"
#include "services/tune.h"

TUNE_OWNER(render);
/* The generated meshlet-size table measures both culling settings. */
TUNE(render, cull, 1, 0, 1);

/* One prepared instance: its mesh, lens, culled clusters and its share of
 * the vertices and rows. */
typedef struct {
    const r3d_lit_mesh_t* mesh;
    const r3d_lens_t* lens;
    const uint16_t* visible; /* the clusters culling kept */
    int visible_count;
    r3d_pipeline_vertex_t* cs;
    r3d_pipeline_rows_t* rows;
} prepared_t;

typedef struct {
    const raster_t* raster;
    const prepared_t* in;
    int first, count; /* clusters of in->visible[], or rows */
    bool clear;       /* the first mesh of a picture clears it; later ones draw over it */
    const r3d_span_writer_t* writers;
    int writer_count;
    int work_index;
    const r3d_pipeline_buffers_t* buffers;
    const gfx_render_target_t* picture; /* the rows being drawn */
    r3d_span_target_t* target;
    int32_t* elapsed_us;
} slice_t;

typedef struct {
    const upscale_t* scale;
    const gfx_render_target_t* picture;
    uint16_t* destination; /* the scale's row 0 */
    uint16_t clear;
    bool scanlined;
    int first, count;
} upscale_slice_t;

typedef struct {
    r3d_span_writer_t writers[GFX_ATTACHMENTS_MAX];
    prepared_t in;
    slice_t mine, other;
    r3d_pipeline_buffers_t buffers;
    gfx_render_target_t picture;
    r3d_span_target_t targets[2];
    r3d_span_small_fill_t small_fill;
    int32_t mine_us, other_us; /* Draw slices stay far below the int32 microsecond limit of 35 minutes. */
    r3d_lens_t* lenses;        /* one per instance, as raster_prepare() left them, right after this */
    uint16_t* window_rows;     /* an upscale window's row map, after the lenses */
} draw_work_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");
_Static_assert(sizeof(upscale_slice_t) <= JOB_CTX_MAX, "upscale_slice_t must fit JOB_CTX_MAX");
_Static_assert(_Alignof(draw_work_t) <= R3D_PIPELINE_WORK_ALIGNMENT, "draw work alignment");
_Static_assert(sizeof(draw_work_t) % _Alignof(r3d_lens_t) == 0, "the lenses follow the draw work");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    r3d_pipeline_transform(s->in->mesh, s->in->lens, s->in->visible + s->first, s->count, s->in->cs, s->in->rows);
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
    assert(raster->band_rows >= 0);
    assert(raster->band_rows == 0 || raster->attachment_count == 0); /* bands draw shaded only */
    const int rows = raster->band_rows == 0 ? raster->height : raster->band_rows;
    gfx_render_target_t picture = {raster->width, 0, rows, GFX_ATTACHMENT_FURTHER + raster->attachment_count, {{0}}};
    for (int i = 0; i < picture.count; i++) {
        picture.attachment[i].bytes_per_pixel = attachment_at(raster, i)->bytes_per_pixel;
    }
    return picture;
}

/* A band picture holds whichever rows were last asked of it, from its first pixel. */
static gfx_render_target_t
rows_of(const raster_t* raster, const gfx_render_target_t* picture, int row0, int row1) {
    assert(row0 >= 0 && row0 <= row1 && row1 <= raster->height);
    if (raster->band_rows == 0) {
        gfx_render_target_t window;
        gfx_render_target_window(&window, picture, row0, row1);
        return window;
    }
    assert(row1 - row0 <= raster->band_rows);
    gfx_render_target_t band = *picture;
    band.row0 = row0;
    band.row1 = row1;
    return band;
}

void
raster_attachment_tag(const r3d_span_writer_t* writer, const gfx_render_target_t* rows, int y, int x_first, int x_last,
                      int32_t z, int32_t dz) {
    const uint16_t* depth = gfx_render_target_depth(rows, y);
    uint16_t* tag = gfx_render_target_row(rows, writer->attachment, y);
    for (int x = x_first; x <= x_last; x++, z += dz) {
        if ((uint16_t)(z >> R3D_DEPTH_SHIFT) == depth[x]) {
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
    const int64_t began_us = timing_now_us();
    const slice_t* s = ctx;
    const raster_t* r = s->raster;
    const prepared_t* in = s->in;
    r3d_span_target_t* target = s->target;
    gfx_render_target_window(&target->rows, s->picture, s->first, s->first + s->count);
    target->writers = s->writers;
    target->writer_count = s->writer_count;
    target->small_fill = &((const draw_work_t*)s->buffers->draw)->small_fill;

    if (s->clear) {
        const size_t pixels = (size_t)s->count * (size_t)r->width;
        for (int i = 0; i < target->rows.count; i++) {
            const raster_attachment_t* a = attachment_at(r, i);
            if (a->clear != NULL) {
                a->clear(a, r, target->rows.attachment[i].pixels, pixels);
            }
        }
    }
    r3d_pipeline_draw(in->mesh, in->lens, in->visible, in->visible_count, in->cs, in->rows, target,
                      s->buffers->work[s->work_index]);
    const int64_t elapsed_us = timing_now_us() - began_us;
    *s->elapsed_us = (int32_t)elapsed_us;
}

static void
resolve_slice(void* ctx) {
    const slice_t* s = ctx;
    const raster_t* r = s->raster;
    gfx_render_target_t rows;
    gfx_render_target_window(&rows, s->picture, s->first, s->first + s->count);
    for (int i = GFX_ATTACHMENT_FURTHER; i < rows.count; i++) {
        const raster_attachment_t* a = attachment_at(r, i);
        if (a->resolve != NULL) {
            a->resolve(a, r, &rows, i);
        }
    }
}

static void
upscale_slice(void* ctx) {
    const upscale_slice_t* s = ctx;
    if (!s->scanlined) {
        upscale_rows(s->scale, s->picture, s->clear, s->destination, s->first, s->count);
    } else {
        upscale_rows_scanlined(s->scale, s->picture, s->clear, s->destination, s->first, s->count);
    }
}

int
raster_vertex_capacity(const raster_t* raster) {
    int total = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        total += raster->instances[i].mesh->vertex_count;
    }
    return total;
}

int
raster_cluster_capacity(const raster_t* raster) {
    int total = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        total += raster->instances[i].mesh->cluster_count;
    }
    return total;
}

size_t
raster_scratch_bytes(const raster_t* raster) {
    assert(raster->instance_count > 0); /* a raster with nothing to draw would clear nothing */
    const gfx_render_target_t picture = raster_picture(raster);
    const size_t prefix =
        (sizeof(r3d_pipeline_vertex_t) * (size_t)raster_vertex_capacity(raster))
        + (sizeof(r3d_pipeline_rows_t) * (size_t)raster_cluster_capacity(raster)) + r3d_pipeline_culled_bytes(raster)
        + (raster->band_rows == 0 ? gfx_render_target_bytes(&picture) : 0)
        + gfx_attachment_bytes(sizeof(uint16_t), raster->destination_width + raster->destination_height, 1);
    return mathi_size_ceil(prefix, R3D_PIPELINE_WORK_ALIGNMENT) + (2 * r3d_pipeline_work_bytes()) + sizeof(draw_work_t)
           + (sizeof(r3d_lens_t) * (size_t)raster->instance_count)
           + (sizeof(uint16_t) * (size_t)raster->destination_height);
}

size_t
raster_band_bytes(const raster_t* raster) {
    assert(raster->band_rows > 0);
    const gfx_render_target_t picture = raster_picture(raster);
    return gfx_render_target_bytes(&picture);
}

/* The view's lens fitted to the size drawn at, so a render scaled more
 * in one axis than the other still shows the same view. */
void
raster_lens(const raster_t* raster, const render_view_t* view, int position_scale, r3d_lens_t* lens) {
    r3d_lens_init(lens, view, position_scale);
    r3d_lens_fit(lens, raster->width, raster->height);
}

/* The lens an instance is drawn with: raster_lens(), or the picture's unfitted
 * when `fitted` is false. */
static void
instance_lens(const raster_t* raster, const r3d_instance_t* instance, const render_view_t* view, bool fitted,
              r3d_lens_t* lens) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    if (fitted) {
        raster_lens(raster, view, mesh->position_scale, lens);
    } else {
        r3d_lens_init(lens, view, mesh->position_scale);
    }
    if (instance->model != NULL) {
        r3d_lens_place(lens, instance->model, mesh->position_scale);
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
    work->lenses = (r3d_lens_t*)(work + 1);
    work->window_rows = (uint16_t*)(work->lenses + raster->instance_count);
    return work;
}

/* Moves `work->in` to instance `index`, from the one before it: each
 * instance's census entry, vertices and rows follow the last one's. */
static void
prepared_at(const raster_t* raster, int index, draw_work_t* work) {
    prepared_t* in = &work->in;
    if (index == 0) {
        const uint16_t* culled = work->buffers.culled;
        *in = (prepared_t){
            .visible = culled + 1, .visible_count = culled[0], .cs = work->buffers.cs, .rows = work->buffers.rows};
    } else {
        const r3d_lit_mesh_t* last = in->mesh;
        const uint16_t* culled = in->visible + last->cluster_count;
        in->visible = culled + 1;
        in->visible_count = culled[0];
        in->cs += last->vertex_count;
        in->rows += last->cluster_count;
    }
    in->mesh = raster->instances[index].mesh;
    in->lens = &work->lenses[index];
}

static inline __attribute__((always_inline)) void
transform_prepared(const raster_t* raster, draw_work_t* work) {
    FRAME_COST_BEGIN(transformed_from);
    const prepared_t* in = &work->in;
    const int half = r3d_pipeline_transform_split(in->mesh, in->visible, in->visible_count);
    slice_t* mine = &work->mine;
    slice_t* other = &work->other;
    *mine = (slice_t){.raster = raster, .in = in, .count = half};
    *other = *mine;
    other->first = half;
    other->count = in->visible_count - half;
    job_run_split(transform_slice, mine, other, sizeof *mine);
    FRAME_COST_END(transformed_from, "r3d.transform");
}

/* Draws the prepared instance `index` over rows [row0, row1) of the picture
 * in hand: core 1 the top slice, this core the rest. */
static inline __attribute__((always_inline)) void
draw_prepared(const raster_t* raster, int index, int row0, int row1, draw_work_t* work) {
    const prepared_t* in = &work->in;
    const int writer_count = instance_writers(raster, index, work->writers);
    FRAME_COST_BEGIN(drawn_from);
    int16_t* retained =
        raster->split != NULL && index < RASTER_SPLIT_INSTANCES ? &raster->split->top_offset[index] : NULL;
    const int share = RASTER_SPLIT_EVEN + (retained == NULL ? 0 : *retained);
    const int mid = row1 - row0 < 2 ? row1
                                    : r3d_pipeline_draw_split(in->mesh, in->visible, in->rows, in->visible_count, row0,
                                                              row1, share);
    slice_t* mine = &work->mine;
    slice_t* other = &work->other;
    *mine = (slice_t){.raster = raster,
                      .in = in,
                      .clear = index == 0,
                      .writers = work->writers,
                      .writer_count = writer_count,
                      .buffers = &work->buffers,
                      .picture = &work->picture};
    *other = *mine;
    mine->elapsed_us = &work->mine_us;
    other->elapsed_us = &work->other_us;
    mine->work_index = 1;
    other->work_index = 0;
    mine->target = &work->targets[1];
    other->target = &work->targets[0];
    mine->first = mid;
    mine->count = row1 - mid;
    other->first = row0;
    other->count = mid - row0;
    job_run_split(draw_slice, mine, other, sizeof *mine);
    /* A band's timings say nothing of the frame's balance: bands keep the
     * share the last whole picture learned. */
    if (retained != NULL && raster->band_rows == 0) {
        *retained =
            (int16_t)(raster_split_update(share, (int64_t)work->other_us, (int64_t)work->mine_us) - RASTER_SPLIT_EVEN);
    }
    FRAME_COST_END(drawn_from, "r3d.draw");
}

/* Frame validation stays outside the inlined draw chain. */
static __attribute__((noinline)) void
assert_view_size(const raster_t* raster, const render_view_t* view) {
    const viewport_t picture = raster_viewport(raster, view->viewport.quarter);
    assert(view->viewport.width == picture.width);
    assert(view->viewport.height == picture.height);
}

/* A NULL stats pointer uses the census list; otherwise each instance is
 * culled. Inlining keeps the draw chain to one entry's stack frame. */
static inline __attribute__((always_inline)) void
prepare_instances(const raster_t* raster, const render_view_t* view, raster_stats_t* stats) {
    assert(raster->instance_count > 0);
    assert_view_size(raster, view);
    for (int i = 0; i < raster->attachment_count; i++) {
        const raster_attachment_t* a = raster->attachments[i];
        if (a->begin != NULL) {
            a->begin(a, raster, view);
        }
    }
    draw_work_t* work = scratch_draw(raster);
    r3d_span_small_fill_resolve(&work->small_fill, view->small_fill);
    uint16_t* culled = work->buffers.culled;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t* lens = &work->lenses[i];
        instance_lens(raster, instance, view, true, lens);
        if (stats != NULL) {
            FRAME_COST_BEGIN(culled_from);
            culled[0] = (uint16_t)cull_instance(instance, lens, culled + 1, stats, work->buffers.work[0]);
            FRAME_COST_END(culled_from, "r3d.cull");
        }
        culled += 1 + (size_t)instance->mesh->cluster_count;
        prepared_at(raster, i, work);
        transform_prepared(raster, work);
    }
}

static inline __attribute__((always_inline)) void
draw_rows(const raster_t* raster, int row0, int row1) {
    draw_work_t* work = scratch_draw(raster);
    work->picture = rows_of(raster, &work->buffers.picture, row0, row1);
    bool resolves = false;
    for (int i = 0; i < raster->attachment_count; i++) {
        resolves = resolves || raster->attachments[i]->resolve != NULL;
    }
    for (int i = 0; i < raster->instance_count; i++) {
        prepared_at(raster, i, work);
        draw_prepared(raster, i, row0, row1, work);
    }
    if (resolves) {
        FRAME_COST_BEGIN(resolved_from);
        const int mid = row0 + ((row1 - row0) / 2);
        work->mine = (slice_t){.raster = raster, .picture = &work->picture, .first = mid, .count = row1 - mid};
        work->other = (slice_t){.raster = raster, .picture = &work->picture, .first = row0, .count = mid - row0};
        job_run_split(resolve_slice, &work->mine, &work->other, sizeof work->mine);
        FRAME_COST_END(resolved_from, "r3d.resolve");
    }
}

raster_stats_t
raster_prepare(const raster_t* raster, const render_view_t* view) {
    raster_stats_t stats = {0, 0};
    prepare_instances(raster, view, &stats);
    return stats;
}

void
raster_prepare_culled(const raster_t* raster, const render_view_t* view) {
    prepare_instances(raster, view, NULL);
}

/* Its own frame, so a whole draw's chain is the deeper of the two stages,
 * not their sum. */
__attribute__((noinline)) void
raster_draw_rows(const raster_t* raster, int row0, int row1) {
    draw_rows(raster, row0, row1);
}

raster_stats_t
raster_draw(const raster_t* raster, const render_view_t* view) {
    raster_stats_t stats = {0, 0};
    prepare_instances(raster, view, &stats);
    raster_draw_rows(raster, 0, raster->height);
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
raster_census(const raster_t* raster, const render_view_t* view) {
    assert(raster->instance_count > 0);
    assert_view_size(raster, view);
    raster_stats_t stats = {0, 0};
    FRAME_COST_BEGIN(counted_from);
    draw_work_t* work = scratch_draw(raster);
    uint16_t* culled = work->buffers.culled;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t lens;
        instance_lens(raster, instance, view, false, &lens);
        culled[0] = (uint16_t)cull_instance(instance, &lens, culled + 1, &stats, work->buffers.work[0]);
        culled += 1 + (size_t)instance->mesh->cluster_count;
    }
    FRAME_COST_END(counted_from, "r3d.census");
    return stats;
}

void
raster_draw_culled(const raster_t* raster, const render_view_t* view) {
    prepare_instances(raster, view, NULL);
    raster_draw_rows(raster, 0, raster->height);
}

resolution_step_t
raster_present_size(resolution_step_t size, camera_present_t present) {
    if (present != CAMERA_PRESENT_NEAREST) {
        size.height = size.height > 1 ? size.height / 2 : 1;
    }
    return size;
}

bool
raster_fits_half_picture(const raster_t* raster) {
    return raster->width > 0 && raster->height > 0 && raster->destination_height > 0
           && 2 * raster->width == raster->destination_width && raster->destination_height % 2 == 0
           && (raster->destination_height / 2) % raster->height == 0;
}

resolution_step_t
raster_compose_size(const raster_t* raster) {
    const int divisor = raster_fits_half_picture(raster) ? 2 : 1;
    return (resolution_step_t){raster->destination_width / divisor, raster->destination_height / divisor};
}

/* Builds the maps for a `width` by `height` destination unless they are
 * already for it. */
static void
upscale_maps(raster_t* raster, int width, int height) {
    assert(raster->upscaled);
    assert(raster->width > 0 && raster->height > 0);
    assert(width >= raster->width && height >= raster->height);
    assert(width <= raster->destination_width && height <= raster->destination_height);
    uint16_t* columns = r3d_pipeline_carve(raster).maps;
    uint16_t* rows = columns + width;
    if (raster->upscale.source_width != raster->width || raster->upscale.source_height != raster->height
        || raster->upscale.destination_width != width || raster->upscale.destination_height != height
        || raster->upscale.columns != columns || raster->upscale.rows != rows) {
        const bool initialized =
            upscale_init(&raster->upscale, raster->width, raster->height, width, height, columns, rows);
        assert(initialized);
    }
}

/* The source rows [*row0, *row1) destination rows [first, end) read. */
static void
upscale_source(const raster_t* raster, int first, int end, int* row0, int* row1) {
    const upscale_t* scale = &raster->upscale;
    assert(0 <= first && first < end && end <= scale->destination_height);
    if (scale->path == UPSCALE_BLOCKS) {
        *row0 = first / scale->factor;
        *row1 = ((end - 1) / scale->factor) + 1;
    } else {
        *row0 = scale->rows[first];
        *row1 = scale->rows[end - 1] + 1;
    }
}

int
raster_band_source_rows(const raster_t* raster, int destination_rows) {
    const int height = raster->height;
    const int destination_height = raster->destination_height;
    return destination_rows == 0 ? 0
                                 : (((destination_rows * height) + destination_height - 1) / destination_height) + 1;
}

/* Upscales source rows [row0, row1) into `destination`: the window as a
 * scale of its own, from its first row and the first source row it reads.
 * Block and scanline runs restart at the window, so it must start on one.
 * Its own frame, so the upscale's never sits under the draw's. */
static __attribute__((noinline)) void
upscale_window(raster_t* raster, gfx_target_t destination, int row0, int row1) {
    FRAME_COST_BEGIN(upscaled_from);
    const int first = destination.y0;
    const int end = first + destination.height;
    upscale_t window = raster->upscale;
    assert(window.path != UPSCALE_BLOCKS || first % window.factor == 0);
    assert(window.scanline_run == 0 || first % window.scanline_run == 0);
    window.source_height = row1 - row0;
    window.destination_height = end - first;
    draw_work_t* work = scratch_draw(raster);
    if (first != 0) {
        for (int y = first; y < end; y++) {
            work->window_rows[y - first] = (uint16_t)(raster->upscale.rows[y] - row0);
        }
        window.rows = work->window_rows;
    }
    const gfx_render_target_t picture = rows_of(raster, &work->buffers.picture, row0, row1);
    const int mid = destination.height / 2;
    upscale_slice_t mine = {.scale = &window,
                            .picture = &picture,
                            .destination = destination.buf,
                            .clear = raster->clear,
                            .scanlined = raster->present == CAMERA_PRESENT_SCANLINE,
                            .first = mid,
                            .count = destination.height - mid};
    upscale_slice_t other = mine;
    other.first = 0;
    other.count = mid;
    job_run_split(upscale_slice, &mine, &other, sizeof mine);
    FRAME_COST_END(upscaled_from, "r3d.upscale");
}

int32_t
raster_compose_rows(raster_t* raster, gfx_target_t destination, int width, int height) {
    assert(destination.buf != NULL && destination.stride == width);
    upscale_maps(raster, width, height);
    const int first = destination.y0;
    const int end = first + destination.height;
    int row0;
    int row1;
    upscale_source(raster, first, end, &row0, &row1);
    const int64_t began_us = timing_now_us();
    if (raster->band_rows != 0) {
        raster_draw_rows(raster, row0, row1);
    }
    const int32_t drawn_us = (int32_t)(timing_now_us() - began_us);
    upscale_window(raster, destination, row0, row1);
    return drawn_us;
}

/* A whole picture is already drawn: only the upscale. */
void
raster_upscale(raster_t* raster, uint16_t* destination, int width, int height) {
    assert(raster->band_rows == 0 && destination != NULL);
    upscale_maps(raster, width, height);
    int row0;
    int row1;
    upscale_source(raster, 0, height, &row0, &row1);
    upscale_window(raster, (gfx_target_t){destination, 0, height, width}, row0, row1);
}
