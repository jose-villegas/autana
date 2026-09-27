#include "render/r3d_lit_frame.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "util/job.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

#define JOB_WAIT_MS   1000
#define SPLIT_BUCKETS 64

typedef struct {
    const r3d_lit_frame_t* frame;
    const r3d_lit_view_t* view;
    int visible_count;
    int first, count; /* clusters of visible[], or rows */
} slice_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    r3d_lit_transform(s->frame->mesh, s->view, s->frame->visible + s->first, s->count, s->frame->cs, s->frame->rows);
}

/* Two panel rows per source row, each pixel written twice as one 32-bit
 * store. A pixel nothing covered (depth still 0) takes the clear colour
 * here, so the colour buffer itself is never cleared. */
static void
double_rows(const r3d_lit_frame_t* f, int first, int count) {
    const int out_width = 2 * f->width;
    for (int y = first; y < first + count; y++) {
        const uint16_t* src = f->color + ((size_t)y * (size_t)f->width);
        const uint16_t* depth = f->depth + ((size_t)y * (size_t)f->width);
        uint32_t* top = (uint32_t*)(f->doubled + ((size_t)(2 * y) * (size_t)out_width));
        uint32_t* bottom = top + f->width;
        for (int x = 0; x < f->width; x++) {
            const uint16_t c = depth[x] != 0 ? src[x] : f->clear;
            const uint32_t pair = ((uint32_t)c << 16) | c;
            top[x] = pair;
            bottom[x] = pair;
        }
    }
}

static void
draw_slice(void* ctx) {
    const slice_t* s = ctx;
    const r3d_lit_frame_t* f = s->frame;
    const size_t offset = (size_t)s->first * (size_t)f->width;
    const size_t pixels = (size_t)s->count * (size_t)f->width;
    uint16_t* color = f->color + offset;
    uint16_t* depth = f->depth + offset;

    if (f->doubled == NULL) {
        for (size_t i = 0; i < pixels; i++) {
            color[i] = f->clear;
        }
    }
    memset(depth, 0, pixels * sizeof(*depth));

    const r3d_span_target_t target = {color, depth, f->width, s->first, s->first + s->count};
    r3d_lit_draw(f->mesh, s->view, f->visible, s->visible_count, f->cs, f->rows, &target);
}

static void
double_slice(void* ctx) {
    const slice_t* s = ctx;
    double_rows(s->frame, s->first, s->count);
}

static void
run_split(job_fn_t fn, slice_t first_half, slice_t second_half) {
    (void)job_run_core1(fn, &second_half, sizeof second_half);
    fn(&first_half);
    const bool done = job_wait(JOB_WAIT_MS);
    assert(done); /* the next stage reads what core 1 wrote */
    (void)done;
}

static size_t
pixels(int width, int height) {
    return (size_t)width * (size_t)height;
}

size_t
r3d_lit_frame_scratch_bytes(const r3d_lit_mesh_t* mesh, int width, int height) {
    return (sizeof(r3d_lit_vertex_t) * (size_t)mesh->vertex_count)
           + ((sizeof(r3d_lit_rows_t) + sizeof(uint16_t)) * (size_t)mesh->cluster_count)
           + (2 * sizeof(uint16_t) * pixels(width, height));
}

/* Widest alignment first, so each part lands aligned after the one before. */
void
r3d_lit_frame_use_scratch(r3d_lit_frame_t* frame, void* scratch) {
    char* p = scratch;
    frame->cs = (r3d_lit_vertex_t*)p;
    p += sizeof(r3d_lit_vertex_t) * (size_t)frame->mesh->vertex_count;
    frame->rows = (r3d_lit_rows_t*)p;
    p += sizeof(r3d_lit_rows_t) * (size_t)frame->mesh->cluster_count;
    frame->color = (uint16_t*)p;
    p += sizeof(uint16_t) * pixels(frame->width, frame->height);
    frame->depth = (uint16_t*)p;
    p += sizeof(uint16_t) * pixels(frame->width, frame->height);
    frame->visible = (uint16_t*)p;
}

/* The row splitting the visible triangles in half, counting each cluster
 * at the middle of its rows - the halves are then drawn by one core each. */
static int
balanced_split_row(const r3d_lit_frame_t* frame, int visible) {
    uint32_t weight[SPLIT_BUCKETS] = {0};
    uint32_t total = 0;
    for (int i = 0; i < visible; i++) {
        const r3d_lit_rows_t* r = &frame->rows[frame->visible[i]];
        const float middle = r->crosses_near ? 0.5F * (float)frame->height : 0.5F * (r->y0 + r->y1);
        int row = (int)middle;
        row = row < 0 ? 0 : (row >= frame->height ? frame->height - 1 : row);
        const uint32_t n = frame->mesh->clusters[frame->visible[i]].triangle_count;
        weight[row * SPLIT_BUCKETS / frame->height] += n;
        total += n;
    }
    uint32_t sum = 0;
    for (int bucket = 0; bucket < SPLIT_BUCKETS; bucket++) {
        sum += weight[bucket];
        if (2 * sum >= total) {
            const int row = (2 * bucket + 1) * frame->height / (2 * SPLIT_BUCKETS);
            return row < 1 ? 1 : row;
        }
    }
    return frame->height / 2;
}

r3d_lit_stats_t
r3d_lit_frame_render(const r3d_lit_frame_t* frame, const r3d_lit_view_t* view) {
    const int visible = r3d_lit_cull_clusters(frame->mesh, view, frame->visible);
    r3d_lit_stats_t stats = {visible, 0};
    for (int i = 0; i < visible; i++) {
        stats.triangles += frame->mesh->clusters[frame->visible[i]].triangle_count;
    }

    const int half = visible / 2;
    run_split(transform_slice, (slice_t){frame, view, visible, 0, half},
              (slice_t){frame, view, visible, half, visible - half});

    const int mid = balanced_split_row(frame, visible);
    run_split(draw_slice, (slice_t){frame, view, visible, mid, frame->height - mid},
              (slice_t){frame, view, visible, 0, mid});
    return stats;
}

void
r3d_lit_frame_double(const r3d_lit_frame_t* frame) {
    const int mid = frame->height / 2;
    run_split(double_slice, (slice_t){frame, NULL, 0, mid, frame->height - mid}, (slice_t){frame, NULL, 0, 0, mid});
}
