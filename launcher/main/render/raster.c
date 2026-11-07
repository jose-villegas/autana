#include "render/raster.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "core/job.h"
#include "core/tune.h"
#include "profile/frame_cost.h"
#include "profile/frame_laps.h"
#include "render/lod/r3d_lod.h"
#include "render/raster_internal.h"
#include "render/upscale.h"

TUNE_OWNER(render);
/* Development overrides of the raster's settings (TUNE_OVERRIDE). */
/* The generated meshlet-size table measures both culling settings. */
TUNE_OVERRIDE(render, cull, 1);
/* 0 encodes depth with the clip plane's near_z: the depth-precision figures
 * render both. */
TUNE_OVERRIDE(render, depth_fit, 1);
/* Off draws one instance a batch. */
TUNE_OVERRIDE(render, batched, 1);
/* Overrides lod.coverage_permille (INT32_MAX lifts the cap); coverage_table.py
 * sets it once per cap. */
TUNE_OVERRIDE(render, lod_coverage_permille, INT32_MAX);

render_settings_t
raster_settings(const raster_t* raster) {
    const render_settings_t s = raster->settings != NULL ? *raster->settings : RENDER_SETTINGS_DEFAULT;
    return (render_settings_t){
        tune_override(cull, s.cull) != 0,
        tune_override(depth_fit, s.depth_fit) != 0,
        tune_override(batched, s.batched) != 0,
        {s.lod.tolerance_px, s.lod.hysteresis_px, tune_override(lod_coverage_permille, s.lod.coverage_permille)}};
}

/* One instance of a batch: its lens, the clusters culling kept, where its
 * vertices and cluster rows start in the shared buffers, and its writers. */
typedef struct {
    const r3d_lit_mesh_t* mesh;
    r3d_lens_t lens;
    const uint16_t* visible;
    int visible_count;
    int vertex_base, cluster_base;
    int writer_count;
    r3d_span_writer_t writers[GFX_ATTACHMENTS_FURTHER_MAX];
} batch_item_t;

int
raster_batch_end(const raster_t* raster, int first) {
    const int most = raster_settings(raster).batched ? RASTER_BATCH_INSTANCES : 1;
    const int vertex_capacity = raster_vertex_capacity(raster);
    const int cluster_capacity = raster_cluster_capacity(raster);
    int vertices = 0;
    int clusters = 0;
    int end = first;
    while (end < raster->instance_count && end - first < most) {
        const r3d_lit_mesh_t* mesh = raster->instances[end].mesh;
        if (vertices + mesh->vertex_count > vertex_capacity || clusters + mesh->cluster_count > cluster_capacity) {
            break;
        }
        vertices += mesh->vertex_count;
        clusters += mesh->cluster_count;
        end++;
    }
    return end;
}

typedef struct {
    batch_item_t items[RASTER_BATCH_INSTANCES];
    r3d_pipeline_part_t parts[RASTER_BATCH_INSTANCES];
    int count;
} batch_t;

typedef struct {
    const raster_t* raster;
    const batch_t* batch;
    int first, count; /* clusters across the batch's items, or rows */
    bool clear;       /* the first batch of a picture clears it; later ones draw over it */
    uint16_t* destination;
    int work_index;
    const raster_buffers_t* buffers;
    r3d_span_target_t* target;
} slice_t;

/* The picture's lens at one position scale, so instances that share it pay
 * only their placement: raster_lens(), or the picture's unfitted when `fitted`
 * is false. */
typedef struct {
    const raster_t* raster;
    const render_view_t* view;
    bool fitted;
    int position_scale; /* that of `lens`; 0 before the first */
    r3d_lens_t lens;
} picture_lens_t;

typedef struct {
    batch_t batch;
    picture_lens_t picture; /* the draw's, kept here so it is off the stack the slices run on */
    slice_t mine, other;
    r3d_span_target_t targets[2];
    r3d_span_small_fill_t small_fill;
    bool skip_hidden; /* the view's */
    raster_buffers_t buffers;
} draw_work_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");
_Static_assert(_Alignof(draw_work_t) <= R3D_PIPELINE_WORK_ALIGNMENT, "draw work alignment");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    FRAME_LAP("r3d.transform.cluster");
    const raster_buffers_t b = raster_carve(s->raster);
    const int end = s->first + s->count;
    int start = 0;
    for (int k = 0; k < s->batch->count && start < end; k++) {
        const batch_item_t* item = &s->batch->items[k];
        const int lo = mathi_max(s->first, start) - start;
        const int hi = mathi_min(end, start + item->visible_count) - start;
        if (lo < hi) {
            r3d_pipeline_transform(item->mesh, &item->lens, item->visible + lo, hi - lo, b.cs + item->vertex_base,
                                   b.rows + item->cluster_base, ((const draw_work_t*)b.draw)->skip_hidden);
        }
        start += item->visible_count;
    }
    FRAME_LAP_END();
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

void
raster_attachment_clear_u16(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

static const raster_attachment_t color_attachment = {GFX_PIXEL_FORMAT, clear_color, NULL, NULL, NULL, NULL};
static const raster_attachment_t depth_attachment = {IMAGE_U16, raster_attachment_clear_u16, NULL, NULL, NULL, NULL};
_Static_assert(R3D_DEPTH_EMPTY == 0, "depth is cleared with memset");

/* Colour, depth, then the caller's, in the order raster_picture() lays them. */
static const raster_attachment_t*
attachment_at(const raster_t* raster, int index) {
    if (index == GFX_ATTACHMENT_COLOR) {
        return &color_attachment;
    }
    return index == GFX_ATTACHMENT_DEPTH ? &depth_attachment : raster->attachments[index - GFX_ATTACHMENT_FURTHER];
}

void
raster_picture(const raster_t* raster, gfx_target_t* picture) {
    assert(raster->attachment_count >= 0 && raster->attachment_count <= GFX_ATTACHMENTS_FURTHER_MAX);
    picture->first_row = 0;
    picture->count = GFX_ATTACHMENT_FURTHER + raster->attachment_count;
    for (int i = 0; i < picture->count; i++) {
        const image_format_t format = attachment_at(raster, i)->format;
        picture->attachment[i] = (image_t){NULL, format, raster->width, raster->height, 0};
    }
}

void
raster_attachment_tag(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last,
                      int32_t z, int32_t dz) {
    const uint16_t* depth = gfx_target_depth(rows, y);
    const image_t* a = &rows->attachment[writer->attachment];
    const size_t offset = (size_t)(y - rows->first_row) * a->pitch;
    uint32_t* wide_tag = (uint32_t*)((char*)a->pixels + offset);
    uint16_t* narrow_tag = (uint16_t*)((char*)a->pixels + offset);
    /* One loop per width, so a pixel tests nothing but its depth. */
    if (a->format == IMAGE_U32) {
        for (int x = x_first; x <= x_last; x++, z += dz) {
            if ((uint16_t)(z >> R3D_DEPTH_SHIFT) == depth[x]) {
                wide_tag[x] = writer->value;
            }
        }
        return;
    }
    for (int x = x_first; x <= x_last; x++, z += dz) {
        if ((uint16_t)(z >> R3D_DEPTH_SHIFT) == depth[x]) {
            narrow_tag[x] = (uint16_t)writer->value;
        }
    }
}

gfx_color_t*
raster_color(const raster_t* raster) {
    const raster_buffers_t b = raster_carve(raster);
    return gfx_target_color(&b.picture, 0);
}

uint16_t*
raster_depth(const raster_t* raster) {
    const raster_buffers_t b = raster_carve(raster);
    return gfx_target_depth(&b.picture, 0);
}

float
raster_depth_near(const raster_t* raster) {
    return *raster_carve(raster).depth_near;
}

static void
draw_slice(void* ctx) {
    const slice_t* s = ctx;
    const raster_t* r = s->raster;
    const raster_buffers_t* b = s->buffers;
    r3d_span_target_t* target = s->target;
    FRAME_LAP("r3d.draw.clear");
    gfx_target_window(&target->rows, &b->picture, s->first, s->first + s->count);
    target->small_fill = &((const draw_work_t*)b->draw)->small_fill;
    target->skip_hidden = ((const draw_work_t*)b->draw)->skip_hidden;

    if (s->clear) {
        const size_t pixels = (size_t)s->count * (size_t)r->width;
        for (int i = 0; i < target->rows.count; i++) {
            const raster_attachment_t* a = attachment_at(r, i);
            if (a->clear != NULL) {
                a->clear(a, r, target->rows.attachment[i].pixels, pixels);
            }
        }
    }
    FRAME_LAP("r3d.draw.cluster");
    for (int k = 0; k < s->batch->count; k++) {
        const batch_item_t* item = &s->batch->items[k];
        target->writers = item->writers;
        target->writer_count = item->writer_count;
        r3d_pipeline_draw(item->mesh, &item->lens, item->visible, item->visible_count, b->cs + item->vertex_base,
                          b->rows + item->cluster_base, target, b->work[s->work_index]);
    }
    FRAME_LAP_END();
}

/* An upscale job's rows: its own type, so the draw's slice_t stays small. */
typedef struct {
    const raster_t* raster;
    int first, count;
    uint16_t* destination;
    uint8_t* seams;              /* the upscale's, or NULL */
    const gfx_target_t* picture; /* the one composed, or NULL for the raster's */
} upscale_slice_t;

static void
upscale_slice(void* ctx) {
    const upscale_slice_t* s = ctx;
    const raster_buffers_t b = raster_carve(s->raster);
    const gfx_target_t* picture = s->picture != NULL ? s->picture : &b.picture;
    if (s->raster->present != CAMERA_PRESENT_SCANLINE) {
        upscale_rows_filtered(&s->raster->upscale, picture, s->raster->clear, s->destination, s->seams, s->first,
                              s->count);
    } else {
        upscale_rows_scanlined(&s->raster->upscale, picture, s->raster->clear, s->destination, s->first, s->count);
    }
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
    return (size_t)((uintptr_t)raster_carve(raster).draw - (uintptr_t)raster->scratch) + sizeof(draw_work_t);
}

/* The view's lens fitted to the size drawn at, so a render scaled more
 * in one axis than the other still shows the same view. */
void
raster_lens(const raster_t* raster, const render_view_t* view, int position_scale, r3d_lens_t* lens) {
    r3d_lens_init(lens, view, position_scale);
    r3d_lens_fit(lens, raster->width, raster->height);
}

/* Set in place: a returned copy would be a temporary on the draw's stack. */
static void
picture_lens_init(picture_lens_t* picture, const raster_t* raster, const render_view_t* view, bool fitted) {
    picture->raster = raster;
    picture->view = view;
    picture->fitted = fitted;
    picture->position_scale = 0;
}

static const r3d_lens_t*
picture_lens_at(picture_lens_t* picture, int position_scale) {
    if (picture->position_scale != position_scale) {
        if (picture->fitted) {
            raster_lens(picture->raster, picture->view, position_scale, &picture->lens);
        } else {
            r3d_lens_init(&picture->lens, picture->view, position_scale);
        }
        picture->position_scale = position_scale;
    }
    return &picture->lens;
}

/* The lens an instance is drawn with: the picture's at its mesh's scale, placed. */
static void
instance_lens(picture_lens_t* picture, const r3d_instance_t* instance, r3d_lens_t* lens) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    *lens = *picture_lens_at(picture, mesh->position_scale);
    if (instance->model != NULL) {
        r3d_lens_place(lens, instance->model, mesh->position_scale);
    }
}

static int
cull_instance(const r3d_instance_t* instance, bool culls, const r3d_lens_t* lens, uint16_t* out, raster_stats_t* stats,
              r3d_pipeline_work_t* work, r3d_depth_fit_t* fit) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    int visible = 0;
    if (culls) {
        visible = r3d_pipeline_cull(mesh, lens, instance->lod_coarse, out, work, fit);
    } else {
        /* Nothing coarse: every cluster of a mesh without levels, or the
         * finest, which levels laid out apart put first. */
        const bool every = (instance->lod_coarse == NULL || !r3d_lod_any(instance->lod_coarse, mesh->lod_group_count))
                           && (mesh->lod_finest_root != 0 || mesh->cluster_lod == NULL);
        const int count = !every || mesh->cluster_lod == NULL ? mesh->cluster_count : mesh->lod_coarse_first;
        for (int i = 0; i < count; i++) {
            if (every || r3d_lod_drawn(mesh, i, instance->lod_coarse)) {
                out[visible++] = (uint16_t)i;
            }
        }
    }
    stats->clusters += visible;
    for (int i = 0; i < visible; i++) {
        stats->triangles += mesh->clusters[out[i]].triangle_count;
    }
    return visible;
}

/* Rounding moves a plane test by about 2^-23 of the coordinates it adds up, so
 * a world box grown by this share of its farthest coordinate and the eye's is
 * never rejected where the instance's own root box, tested in mesh ticks
 * through its placed lens, is kept. */
#define WORLD_SLACK (1.0F / 1024.0F)

/* Whether `box`, in world units, is wholly outside one of `planes`, the
 * picture's lens at a position scale of 1 through r3d_lens_frustum(), once
 * grown by WORLD_SLACK; `eye_reach` is the view position's largest
 * coordinate, in magnitude. */
static bool
world_bounds_outside(box3f_t box, const planef_t planes[PLANE_FRUSTUM_COUNT], float eye_reach) {
    const float coords[] = {box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z};
    float reach = eye_reach;
    for (size_t i = 0; i < sizeof coords / sizeof coords[0]; i++) {
        reach = mathf_max(reach, mathf_abs(coords[i]));
    }
    const float s = reach * WORLD_SLACK;
    const box3f_t grown = {vec3f_sub(box.lo, (vec3f_t){s, s, s}), vec3f_add(box.hi, (vec3f_t){s, s, s})};
    unsigned mask = (1U << PLANE_FRUSTUM_COUNT) - 1U;
    return box3f_outside_planes(grown, planes, PLANE_FRUSTUM_COUNT, &mask);
}

/* The picture's planes in world units and the eye's reach that
 * world_bounds_outside() takes. */
typedef struct {
    planef_t planes[PLANE_FRUSTUM_COUNT];
    float eye_reach;
    bool cull; /* the draw's settings.cull: off skips no instance */
} world_cull_t;

static void
world_cull_init(world_cull_t* world, picture_lens_t* picture, const render_view_t* view, bool cull) {
    world->cull = cull;
    r3d_lens_frustum(picture_lens_at(picture, 1), world->planes);
    const vec3f_t eye = view->position;
    world->eye_reach = mathf_max(mathf_abs(eye.x), mathf_max(mathf_abs(eye.y), mathf_abs(eye.z)));
}

/* Whether culling skips `instance` whole: its world box is outside the picture. */
static bool
world_cull_skips(const world_cull_t* world, const r3d_instance_t* instance) {
    return world->cull && instance->world_bounds != NULL
           && world_bounds_outside(*instance->world_bounds, world->planes, world->eye_reach);
}

/* An instance culled whole keeps the bits it last had, a cut as whole as any:
 * it is not drawn, and the frame it comes back in picks again before its draw. */
void
raster_pick_levels(const raster_t* raster, const render_view_t* view) {
    picture_lens_t picture;
    picture_lens_init(&picture, raster, view, false);
    world_cull_t world;
    const render_settings_t settings = raster_settings(raster);
    world_cull_init(&world, &picture, view, settings.cull);
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        if (instance->lod_coarse == NULL || instance->mesh->lod_group_count == 0
            || world_cull_skips(&world, instance)) {
            continue;
        }
        r3d_lens_t lens;
        instance_lens(&picture, instance, &lens);
        r3d_lod_pick(instance->mesh, &lens, &settings.lod, instance->lod_coarse);
    }
}

/* Culls every instance into the census list and returns the depth scale
 * the picture is drawn with: the fit of the nearest bound of what was kept,
 * and near_z when nothing was. The fit's power of two keeps the scale still
 * while the bound drifts, so ties do not flip from frame to frame, and the
 * same at every size. Not inlined, so the world planes stay off the
 * draw's stack. */
static __attribute__((noinline)) float
cull_instances(const raster_t* raster, const render_view_t* view, bool fitted, raster_stats_t* stats,
               draw_work_t* work) {
    uint16_t* culled = work->buffers.culled;
    const render_settings_t settings = raster_settings(raster);
    r3d_depth_fit_t fit = {view->lens.near_z, INFINITY};
    picture_lens_t picture;
    picture_lens_init(&picture, raster, view, fitted);
    world_cull_t world;
    world_cull_init(&world, &picture, view, settings.cull);
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        if (world_cull_skips(&world, instance)) {
            culled[0] = 0;
        } else {
            r3d_lens_t lens;
            instance_lens(&picture, instance, &lens);
            culled[0] = (uint16_t)cull_instance(instance, settings.cull, &lens, culled + 1, stats,
                                                work->buffers.work[0], settings.depth_fit ? &fit : NULL);
        }
        culled += 1 + (size_t)instance->mesh->cluster_count;
    }
    return fit.depth_near < INFINITY ? fit.depth_near : view->lens.near_z;
}

/* The writers of the attachments that write while instance `index` is
 * drawn; returns how many. */
static int
instance_writers(const raster_t* raster, int index, r3d_span_writer_t out[GFX_ATTACHMENTS_FURTHER_MAX]) {
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
    const raster_buffers_t b = raster_carve(raster);
    draw_work_t* work = b.draw;
    work->buffers = b;
    return work;
}

/* Draws batch `index`: one transform fork/join over every item's clusters,
 * then one draw fork/join in which each core draws every item in its rows. */
static inline __attribute__((always_inline)) void
draw_batch(const raster_t* raster, int index, draw_work_t* work) {
    batch_t* batch = &work->batch;
    int count = 0;
    for (int k = 0; k < batch->count; k++) {
        const batch_item_t* item = &batch->items[k];
        batch->parts[k] = (r3d_pipeline_part_t){item->mesh, item->visible, item->visible_count,
                                                work->buffers.rows + item->cluster_base};
        count += item->visible_count;
    }
    /* Batches past the retained state split evenly and forget it. */
    const bool retained = raster->split != NULL && index < RASTER_SPLIT_INSTANCES;
    int16_t* transform_share = retained ? &raster->split->transform_offset[index] : NULL;
    int16_t* draw_share = retained ? &raster->split->top_offset[index] : NULL;
    FRAME_COST_BEGIN(transformed_from);
    FRAME_LAP("r3d.transform.split");
    const int half = r3d_pipeline_transform_split(batch->parts, batch->count,
                                                  JOB_SHARE_PERMILLE - job_share_permille(transform_share));
    slice_t* mine = &work->mine;
    slice_t* other = &work->other;
    *mine = (slice_t){.raster = raster, .batch = batch, .count = half, .clear = index == 0, .buffers = &work->buffers};
    *other = *mine;
    other->first = half;
    other->count = count - half;
    FRAME_LAP_END();
    job_run_steered(transform_share, transform_slice, mine, other, sizeof *mine);
    FRAME_LAP_SINCE("r3d.transform.wait");
    FRAME_COST_END(transformed_from, "r3d.transform");

    FRAME_COST_BEGIN(drawn_from);
    FRAME_LAP("r3d.draw.split");
    const int mid = r3d_pipeline_draw_split(batch->parts, batch->count, raster->height, job_share_permille(draw_share));
    mine->work_index = 1;
    other->work_index = 0;
    mine->target = &work->targets[1];
    other->target = &work->targets[0];
    mine->first = mid;
    mine->count = raster->height - mid;
    other->first = 0;
    other->count = mid;
    FRAME_LAP_END();
    job_run_steered(draw_share, draw_slice, mine, other, sizeof *mine);
    FRAME_LAP_SINCE("r3d.draw.wait");
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
draw_instances(const raster_t* raster, const render_view_t* view, raster_stats_t* stats) {
    assert(raster->instance_count > 0);
    assert_view_size(raster, view);
    draw_work_t* work = scratch_draw(raster);
    /* Every instance is culled before any is drawn: they share one depth scale. */
    if (stats != NULL) {
        FRAME_COST_BEGIN(culled_from);
        *work->buffers.depth_near = cull_instances(raster, view, true, stats, work);
        FRAME_COST_END(culled_from, "r3d.cull");
    }
    for (int i = 0; i < raster->attachment_count; i++) {
        const raster_attachment_t* a = raster->attachments[i];
        if (a->begin != NULL) {
            a->begin(a, raster, view);
        }
    }
    r3d_span_small_fill_resolve(&work->small_fill, view->small_fill);
    work->skip_hidden = view->skip_hidden;
    /* A list drawn with no census, or rewritten without its scale, fails here. */
    assert(*work->buffers.depth_near >= view->lens.near_z);
    uint16_t* culled = work->buffers.culled;
    batch_t* batch = &work->batch;
    picture_lens_t* picture = &work->picture;
    picture_lens_init(picture, raster, view, true);
    for (int first = 0, index = 0; first < raster->instance_count; index++) {
        const int end = raster_batch_end(raster, first);
        int vertices = 0;
        int clusters = 0;
        for (batch->count = 0; first < end; first++) {
            const r3d_instance_t* instance = &raster->instances[first];
            const r3d_lit_mesh_t* mesh = instance->mesh;
            /* A writer may number clusters across every instance, so it sees
             * this one even when it kept nothing; that one's slot is reused. */
            batch_item_t* item = &batch->items[batch->count];
            item->writer_count = instance_writers(raster, first, item->writers);
            if (culled[0] > 0) {
                batch->count++;
                item->mesh = mesh;
                instance_lens(picture, instance, &item->lens);
                r3d_lens_set_depth_near(&item->lens, *work->buffers.depth_near);
                item->visible = culled + 1;
                item->visible_count = culled[0];
                item->vertex_base = vertices;
                item->cluster_base = clusters;
            }
            vertices += mesh->vertex_count;
            clusters += mesh->cluster_count;
            culled += 1 + (size_t)mesh->cluster_count;
        }
        draw_batch(raster, index, work);
    }
}

raster_stats_t
raster_draw(const raster_t* raster, const render_view_t* view) {
    raster_stats_t stats = {0, 0};
    draw_instances(raster, view, &stats);
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
    *work->buffers.depth_near = cull_instances(raster, view, false, &stats, work);
    FRAME_COST_END(counted_from, "r3d.census");
    return stats;
}

void
raster_draw_culled(const raster_t* raster, const render_view_t* view) {
    draw_instances(raster, view, NULL);
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
           && raster->height <= raster->destination_height / 2;
}

resolution_step_t
raster_compose_size(const raster_t* raster) {
    const int divisor = raster_fits_half_picture(raster) ? 2 : 1;
    return (resolution_step_t){raster->destination_width / divisor, raster->destination_height / divisor};
}

void
raster_upscale(raster_t* raster, uint16_t* destination, int width, int height) {
    raster_upscale_picture(raster, NULL, destination, NULL, width, height);
}

void
raster_upscale_picture(raster_t* raster, const gfx_target_t* picture, uint16_t* destination, uint8_t* seams, int width,
                       int height) {
    assert(raster->upscaled && destination != NULL);
    assert(picture == NULL
           || (picture->attachment[0].width == raster->width && picture->attachment[0].height == raster->height));
    assert(raster->width > 0 && raster->height > 0);
    assert(width >= raster->width && height >= raster->height);
    FRAME_COST_BEGIN(upscaled_from);
    uint16_t* columns = raster_carve(raster).upscale;
    uint16_t* rows = columns + width;
    if (raster->upscale.source_width != raster->width || raster->upscale.source_height != raster->height
        || raster->upscale.destination_width != width || raster->upscale.destination_height != height
        || raster->upscale.columns != columns || raster->upscale.rows != rows) {
        const bool initialized =
            upscale_init(&raster->upscale, raster->width, raster->height, width, height, columns, rows);
        assert(initialized);
        upscale_init_blends(&raster->upscale, rows + height);
    }
    int16_t* share = raster->split != NULL ? &raster->split->upscale_offset : NULL;
    const int mid = job_share_core1_count(share, height);
    upscale_slice_t mine = {.raster = raster,
                            .first = mid,
                            .count = height - mid,
                            .destination = destination,
                            .seams = seams,
                            .picture = picture};
    const upscale_slice_t other = {
        .raster = raster, .count = mid, .destination = destination, .seams = seams, .picture = picture};
    job_run_steered(share, upscale_slice, &mine, &other, sizeof mine);
    FRAME_COST_END(upscaled_from, "r3d.upscale");
}
