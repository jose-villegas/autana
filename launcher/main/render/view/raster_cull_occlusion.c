#include "render/view/raster_cull_occlusion.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "core/job.h"
#include "math/geometry/box3f.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "render/r3d_span_internal.h"
#include "render/view/r3d_pipeline_count.h"
#include "render/view/raster_cull_view.h"

/* Core bands: the draw's two row slices, each its own depth. */
#define BANDS        2
/* A box's corners, box3f_corner()'s k, each bit an axis. */
#define BOX_CORNERS  8
/* Added to a cluster's nearest depth, over the fill's rounding of its
 * interpolated depth; a bound too tight shows as unsafe. */
#define DEPTH_MARGIN 1
/* Where a projected corner is clamped, in subpixels, before it is rounded:
 * far past any picture, inside int32_t. */
#define CORNER_RANGE ((float)(1 << 24))

void
raster_occlusion_begin(raster_occlusion_t* o, gfx_color_t* color, uint16_t* depth, int width, int height,
                       small_fill_t small_fill) {
    memset(o, 0, sizeof *o);
    memset(depth, 0, (size_t)width * (size_t)height * sizeof *depth);
    _Static_assert(R3D_DEPTH_EMPTY == 0, "depth is cleared with memset");
    o->picture = r3d_span_target(color, depth, width, 0, height);
    r3d_span_small_fill_resolve(&o->small_fill, small_fill);
    o->picture.small_fill = &o->small_fill;
    o->height = height;
}

/* The 16-bit depth no part of a cluster whose nearest point is `iz` goes past. */
static int32_t
depth_bound(float iz) {
    return (int32_t)ceilf(iz * (float)R3D_DEPTH_NEAREST) + DEPTH_MARGIN;
}

/* The pixel centres between subpixel positions lo and hi in both axes, in `band`. */
static recti_t
centres_box(const r3d_span_target_t* band, int32_t lo_x, int32_t lo_y, int32_t hi_x, int32_t hi_y) {
    const int x = r3d_span_first_centre(lo_x);
    const int y = r3d_span_first_centre(lo_y);
    return r3d_span_window_box(band, (recti_t){x, y, r3d_span_first_centre(hi_x) - x, r3d_span_first_centre(hi_y) - y});
}

/* r3d_span_hidden()'s scan, counting the pixels it reads before it stops. */
static bool
scan_hidden(const r3d_span_target_t* band, int32_t bound, recti_t box, int64_t* read) {
    for (int y = box.y; y < recti_bottom(box); y++) {
        const uint16_t* depth = gfx_render_target_depth(&band->rows, y);
        for (int x = box.x; x < recti_right(box); x++) {
            (*read)++;
            if (depth[x] < bound) {
                return false;
            }
        }
    }
    return true;
}

/* The tiles `box` touches, each by its farthest depth, counting the tiles read. */
static bool
tiles_hidden(const r3d_span_target_t* band, int32_t bound, recti_t box, int64_t* read) {
    if (recti_is_empty(box)) {
        return true;
    }
    const int t = RASTER_OCCLUSION_TILE;
    for (int ty = box.y / t; ty <= (recti_bottom(box) - 1) / t; ty++) {
        for (int tx = box.x / t; tx <= (recti_right(box) - 1) / t; tx++) {
            (*read)++;
            int farthest = R3D_DEPTH_NEAREST;
            for (int y = mathi_max(ty * t, band->rows.row0); y < mathi_min((ty + 1) * t, band->rows.row1); y++) {
                const uint16_t* depth = gfx_render_target_depth(&band->rows, y);
                for (int x = tx * t; x < mathi_min((tx + 1) * t, band->rows.width); x++) {
                    farthest = depth[x] < farthest ? depth[x] : farthest;
                }
            }
            if (farthest < bound) {
                return false;
            }
        }
    }
    return true;
}

/* The box of a cluster's projected vertices, in subpixels, and its nearest depth. */
typedef struct {
    int32_t lo_x, lo_y, hi_x, hi_y;
    float iz;
    bool valid; /* every point in front and snapped */
} screen_box_t;

static screen_box_t
vertex_box(const r3d_lit_cluster_t* c, const r3d_pipeline_vertex_t* cs, const r3d_pipeline_rows_t* rows) {
    screen_box_t s = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, 0.0F, !rows->unbounded};
    for (int v = c->vertex_first; s.valid && v < c->vertex_first + c->vertex_count; v++) {
        s.lo_x = cs[v].sx < s.lo_x ? cs[v].sx : s.lo_x;
        s.hi_x = cs[v].sx > s.hi_x ? cs[v].sx : s.hi_x;
        s.lo_y = cs[v].sy < s.lo_y ? cs[v].sy : s.lo_y;
        s.hi_y = cs[v].sy > s.hi_y ? cs[v].sy : s.hi_y;
        s.iz = cs[v].iz > s.iz ? cs[v].iz : s.iz;
    }
    return s;
}

/* The same over the cluster's bounding box's corners, widened by the half
 * subpixel a vertex snaps by. */
static screen_box_t
corner_box(const r3d_lit_cluster_t* c, const r3d_lens_t* lens) {
    screen_box_t s = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN, 0.0F, true};
    const box3f_t box = box3f_from_box3s(c->bounds, 1.0F);
    for (int k = 0; k < BOX_CORNERS; k++) {
        const vec3f_t l = mat4f_apply(&lens->m, box3f_corner(box, k));
        if (l.z <= lens->project.near_z) {
            s.valid = false;
            return s;
        }
        const float inv = (float)R3D_SUBPIXEL / l.z;
        const float x = mathf_clamp(lens->snap_cx + (l.x * inv) - R3D_SNAP_BIAS, -CORNER_RANGE, CORNER_RANGE);
        const float y = mathf_clamp(lens->snap_cy + (l.y * inv) - R3D_SNAP_BIAS, -CORNER_RANGE, CORNER_RANGE);
        s.lo_x = mathi_min(s.lo_x, (int32_t)floorf(x - 0.5F));
        s.hi_x = mathi_max(s.hi_x, (int32_t)ceilf(x + 0.5F));
        s.lo_y = mathi_min(s.lo_y, (int32_t)floorf(y - 0.5F));
        s.hi_y = mathi_max(s.hi_y, (int32_t)ceilf(y + 0.5F));
        s.iz = mathf_max(s.iz, lens->near_subpixels * inv);
    }
    return s;
}

/* Whether each bound skips the cluster in `band`. */
static void
test_bounds(raster_occlusion_t* o, const r3d_span_target_t* band, const screen_box_t* vertices,
            const screen_box_t* corners, bool skip[RASTER_OCCLUSION_BOUNDS]) {
    int64_t* read = o->frame.read;
    if (vertices->valid) {
        const recti_t box = centres_box(band, vertices->lo_x, vertices->lo_y, vertices->hi_x, vertices->hi_y);
        skip[RASTER_OCCLUSION_RECT] = scan_hidden(band, depth_bound(vertices->iz), box, &read[RASTER_OCCLUSION_RECT]);
    }
    if (corners->valid) {
        const recti_t box = centres_box(band, corners->lo_x, corners->lo_y, corners->hi_x, corners->hi_y);
        const int32_t bound = depth_bound(corners->iz);
        skip[RASTER_OCCLUSION_CORNERS] = scan_hidden(band, bound, box, &read[RASTER_OCCLUSION_CORNERS]);
        skip[RASTER_OCCLUSION_TILES] = tiles_hidden(band, bound, box, &read[RASTER_OCCLUSION_TILES]);
    }
}

/* Every depth of `band` summed: a draw only raises depth, so any write raises it. */
static int64_t
depth_sum(const r3d_span_target_t* band) {
    int64_t sum = 0;
    for (int y = band->rows.row0; y < band->rows.row1; y++) {
        const uint16_t* depth = gfx_render_target_depth(&band->rows, y);
        for (int x = 0; x < band->rows.width; x++) {
            sum += depth[x];
        }
    }
    return sum;
}

void
raster_occlusion_instance(raster_occlusion_t* o, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens,
                          const raster_buffers_t* b, bool culls, const uint8_t* fate) {
    uint16_t* order = b->culled;
    int count = mesh->cluster_count;
    if (culls) {
        count = r3d_pipeline_cull(mesh, lens, order, b->work[0]);
    } else {
        for (int c = 0; c < count; c++) {
            order[c] = (uint16_t)c;
        }
    }
    for (int c = 0; c < mesh->cluster_count; c++) {
        o->frame.hidden += fate[c] == RASTER_FATE_HIDDEN;
    }
    r3d_pipeline_transform(mesh, lens, order, count, b->cs, b->rows);
    /* The frame's split is steered by the cores' times; an even share of the
     * estimated weight stands in for it. */
    const int mid = r3d_pipeline_draw_split(mesh, order, b->rows, count, o->height, JOB_SHARE_PERMILLE / 2);
    r3d_span_target_t bands[BANDS] = {o->picture, o->picture};
    gfx_render_target_window(&bands[0].rows, &o->picture.rows, 0, mid);
    gfx_render_target_window(&bands[1].rows, &o->picture.rows, mid, o->height);
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[order[i]];
        const screen_box_t vertices = vertex_box(c, b->cs, &b->rows[order[i]]);
        const screen_box_t corners = corner_box(c, lens);
        bool all[RASTER_OCCLUSION_BOUNDS] = {true, true, true};
        bool reached = false;
        for (int k = 0; k < BANDS; k++) {
            r3d_pipeline_counts_t counts = {0};
            r3d_pipeline_count_triangles(mesh, lens, &order[i], 1, b->cs, &bands[k], &counts, NULL);
            if (counts.triangles_set_up + counts.triangles_clipped == 0) {
                continue;
            }
            reached = true;
            o->frame.triangles_set_up += counts.triangles_set_up;
            bool skip[RASTER_OCCLUSION_BOUNDS] = {false};
            test_bounds(o, &bands[k], &vertices, &corners, skip);
            bool any = false;
            for (int n = 0; n < RASTER_OCCLUSION_BOUNDS; n++) {
                o->frame.removed[n] += skip[n] ? counts.triangles_set_up : 0;
                all[n] = all[n] && skip[n];
                any = any || skip[n];
            }
            const int64_t before = any ? depth_sum(&bands[k]) : 0;
            r3d_pipeline_draw(mesh, lens, &order[i], 1, b->cs, b->rows, &bands[k], b->work[0]);
            const bool wrote = any && depth_sum(&bands[k]) != before;
            for (int n = 0; n < RASTER_OCCLUSION_BOUNDS; n++) {
                o->frame.unsafe[n] += skip[n] && wrote;
            }
        }
        for (int n = 0; reached && n < RASTER_OCCLUSION_BOUNDS; n++) {
            o->frame.caught[n] += all[n];
            o->frame.hidden_caught[n] += all[n] && fate[order[i]] == RASTER_FATE_HIDDEN;
        }
    }
}
