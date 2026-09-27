#include "lit_frame.h"

#include <string.h>

#include "util/job.h"

#define JOB_WAIT_MS 1000

typedef struct {
    const lit_frame_t* frame;
    const lit_view_t* view;
    int visible_count;
    int first, count; /* clusters of visible[], or rows */
} slice_t;

_Static_assert(sizeof(slice_t) <= JOB_CTX_MAX, "slice_t must fit JOB_CTX_MAX");

static void
transform_slice(void* ctx) {
    const slice_t* s = ctx;
    lit_transform(s->frame->mesh, s->view, s->frame->visible + s->first, s->count, s->frame->cs, s->frame->rows);
}

/* Two panel rows per source row, each pixel written twice as one 32-bit
 * store. */
static void
double_rows(const lit_frame_t* f, int first, int count) {
    const int out_width = 2 * f->width;
    for (int y = first; y < first + count; y++) {
        const gfx_color_t* src = f->color + (size_t)y * (size_t)f->width;
        uint32_t* top = (uint32_t*)(f->doubled + (size_t)(2 * y) * (size_t)out_width);
        uint32_t* bottom = top + f->width;
        for (int x = 0; x < f->width; x++) {
            const uint32_t pair = ((uint32_t)src[x] << 16) | src[x];
            top[x] = pair;
            bottom[x] = pair;
        }
    }
}

static void
draw_slice(void* ctx) {
    const slice_t* s = ctx;
    const lit_frame_t* f = s->frame;
    const size_t offset = (size_t)s->first * (size_t)f->width;
    const size_t pixels = (size_t)s->count * (size_t)f->width;
    gfx_color_t* color = f->color + offset;
    uint16_t* depth = f->depth + offset;

    for (size_t i = 0; i < pixels; i++) {
        color[i] = f->clear;
    }
    memset(depth, 0, pixels * sizeof(*depth));

    const span_target_t target = {color, depth, f->width, s->first, s->first + s->count};
    lit_draw(f->mesh, s->view, f->visible, s->visible_count, f->cs, f->rows, &target);
    if (f->doubled != NULL) {
        double_rows(f, s->first, s->count);
    }
}

static void
run_split(job_fn_t fn, slice_t first_half, slice_t second_half) {
    (void)job_run_core1(fn, &second_half, sizeof second_half);
    fn(&first_half);
    (void)job_wait(JOB_WAIT_MS);
}

size_t
lit_frame_scratch_bytes(const lit_mesh_t* mesh) {
    return sizeof(lit_cs_vertex_t) * (size_t)mesh->vertex_count
           + (sizeof(lit_cluster_rows_t) + sizeof(uint16_t)) * (size_t)mesh->cluster_count;
}

/* Widest alignment first, so each part lands aligned after the one before. */
void
lit_frame_use_scratch(lit_frame_t* frame, void* scratch) {
    char* p = scratch;
    frame->cs = (lit_cs_vertex_t*)p;
    p += sizeof(lit_cs_vertex_t) * (size_t)frame->mesh->vertex_count;
    frame->rows = (lit_cluster_rows_t*)p;
    p += sizeof(lit_cluster_rows_t) * (size_t)frame->mesh->cluster_count;
    frame->visible = (uint16_t*)p;
}

/* The row splitting the visible triangles in half, counting each cluster
 * at the middle of its rows - the halves are then drawn by one core each. */
static int
balanced_split_row(const lit_frame_t* frame, int visible) {
    uint16_t weight[LIT_FRAME_MAX_HEIGHT] = {0}; /* a mesh holds under 65536 triangles */
    int total = 0;
    for (int i = 0; i < visible; i++) {
        const lit_cluster_rows_t* r = &frame->rows[frame->visible[i]];
        const float middle = r->crosses_near ? 0.5f * (float)frame->height : 0.5f * (r->y0 + r->y1);
        int row = (int)middle;
        row = row < 0 ? 0 : (row >= frame->height ? frame->height - 1 : row);
        const int n = frame->mesh->clusters[frame->visible[i]].triangle_count;
        weight[row] = (uint16_t)(weight[row] + n);
        total += n;
    }
    int sum = 0;
    for (int row = 0; row < frame->height; row++) {
        sum += weight[row];
        if (2 * sum >= total) {
            return row < 1 ? 1 : row;
        }
    }
    return frame->height / 2;
}

lit_frame_stats_t
lit_frame_render(const lit_frame_t* frame, const lit_view_t* view) {
    const int visible = lit_cull_clusters(frame->mesh, view, frame->visible);
    lit_frame_stats_t stats = {visible, 0};
    for (int i = 0; i < visible; i++) {
        stats.triangles += frame->mesh->clusters[frame->visible[i]].triangle_count;
    }

    const int half = visible / 2;
    run_split(transform_slice, (slice_t){frame, view, visible, 0, half},
              (slice_t){frame, view, visible, half, visible - half});

    const int mid = frame->height <= LIT_FRAME_MAX_HEIGHT ? balanced_split_row(frame, visible) : frame->height / 2;
    run_split(draw_slice, (slice_t){frame, view, visible, mid, frame->height - mid},
              (slice_t){frame, view, visible, 0, mid});
    return stats;
}
