#include "render/raster_motion.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"
#include "render/raster.h"
#include "render/raster_show.h"
#include "util/scalar/mathf.h"

/* While drawing, the attachment holds each pixel's tag: 0 for anything
 * that did not move, i + 1 for instance i that did. Resolving turns the tag
 * and the depth into the motion in place. */
typedef uint16_t tag_t;
_Static_assert(sizeof(tag_t) == sizeof(raster_motion_px_t), "a tag resolves in place");

#define DEPTH_SCALE (1.0F / (float)R3D_DEPTH_NEAREST)

/* World to lens space for `view`, the raster's own lens at one tick to the
 * unit, then carried by `placement` when it is not NULL, as r3d_lens_place()
 * does for a placed mesh. */
static void
lens_map(const render_view_t* view, const raster_t* raster, const r3d_placement_t* placement, r3d_lens_t* lens) {
    raster_lens(raster, view, 1, lens);
    if (placement != NULL) {
        r3d_lens_place(lens, placement, 1);
    }
}

static int
seen_index(const raster_motion_t* m, const r3d_placement_t* key) {
    for (int i = 0; i < m->seen_count; i++) {
        if (m->seen_key[i] == key) {
            return i;
        }
    }
    return -1;
}

/* Every instance that moved since the previous picture gets its own map. */
static void
map_instances(raster_motion_t* m, const raster_t* raster, const render_view_t* view) {
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_placement_t* p = raster->instances[i].placement;
        const int seen = p == NULL ? -1 : seen_index(m, p);
        if (seen >= 0 && memcmp(&m->seen[seen], p, sizeof(*p)) != 0) {
            r3d_lens_t before;
            r3d_lens_t now;
            lens_map(&m->previous, raster, &m->seen[seen], &before);
            lens_map(view, raster, p, &now);
            m->map[i + 1] = mat4f_mul_affine(before.m, mat4f_invert_affine(now.m));
            m->moved[i] = true;
            m->first_moved = m->first_moved < 0 ? i : m->first_moved;
        }
    }
}

/* This picture's view and placements, for the next one. */
static void
remember(raster_motion_t* m, const raster_t* raster, const render_view_t* view) {
    m->previous = *view;
    m->has_previous = true;
    m->seen_count = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_placement_t* p = raster->instances[i].placement;
        if (p != NULL) {
            m->seen_key[m->seen_count] = p;
            m->seen[m->seen_count++] = *p;
        }
    }
}

/* The maps for this picture, then this picture kept as the next one's
 * previous. A placement is known by its address from picture to picture. */
static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    raster_motion_t* m = self->state;
    assert(raster->instance_count <= RASTER_MOTION_INSTANCES_MAX);
    r3d_lens_t now;
    lens_map(view, raster, NULL, &now);
    m->center_x = now.center_x;
    m->center_y = now.center_y;
    m->near_z = now.near_z;
    m->known = m->has_previous;
    m->first_moved = -1;
    memset(m->moved, 0, sizeof(m->moved));
    if (m->known) {
        render_view_refit(&m->previous, view->viewport);
        r3d_lens_t before;
        lens_map(&m->previous, raster, NULL, &before);
        m->map[0] = mat4f_mul_affine(before.m, mat4f_invert_affine(now.m));
        map_instances(m, raster, view);
    }
    remember(m, raster, view);
}

static void
clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(tag_t));
}

/* From the first instance that moved on: before it every pixel keeps the
 * tag 0 it was cleared to, so on a picture where nothing moved nothing is
 * written while drawing. */
static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    const raster_motion_t* m = self->state;
    if (m->first_moved < 0 || instance < m->first_moved) {
        return false;
    }
    out->span = raster_attachment_tag;
    out->value = m->moved[instance] ? (uint32_t)instance + 1U : 0U;
    return true;
}

static int8_t
half_pixels(float pixels) {
    const float half = pixels * 2.0F;
    if (!(half > -127.5F && half < 127.5F)) {
        return RASTER_MOTION_UNKNOWN; /* a NaN too */
    }
    return (int8_t)(int32_t)(half + (half < 0.0F ? -0.5F : 0.5F));
}

static const raster_motion_px_t unknown = {RASTER_MOTION_UNKNOWN, RASTER_MOTION_UNKNOWN};

/* A map along one row: its three planes in u and w, the row's v folded in,
 * held in locals so a pixel loads nothing but its own depth and tag. */
typedef struct {
    float u[3], at[3], w[3];
} row_map_t;

static void
row_map(const mat4f_t* a, float v, row_map_t* r) {
    for (int i = 0; i < 3; i++) {
        r->u[i] = a->m[i][0];
        r->at[i] = (a->m[i][1] * v) + a->m[i][2];
        r->w[i] = a->m[i][3];
    }
}

/* Lens space is pixels at unit depth: a pixel at (u, v) from the centre
 * with inverse depth iz is the point (u, v, 1) near_z / iz. Through a map
 * (A, t) it lands at (A (u, v, 1) + t iz / near_z) near_z / iz, whose
 * screen position is the first two over the third; w is iz / near_z. */
static inline raster_motion_px_t
moved(const row_map_t* r, float u, float v, float w) {
    const float pz = (r->u[2] * u) + r->at[2] + (r->w[2] * w);
    if (!(pz > 0.0F)) {
        return unknown;
    }
    const float inv = mathf_recip(pz);
    const int8_t dx = half_pixels((((r->u[0] * u) + r->at[0] + (r->w[0] * w)) * inv) - u);
    const int8_t dy = half_pixels((((r->u[1] * u) + r->at[1] + (r->w[1] * w)) * inv) - v);
    return dx == RASTER_MOTION_UNKNOWN || dy == RASTER_MOTION_UNKNOWN ? unknown : (raster_motion_px_t){dx, dy};
}

static void
resolve_row(const raster_motion_t* m, const uint16_t* depth, raster_motion_px_t* out, int width, float v) {
    const tag_t* tag = (const tag_t*)out;
    const float to_w = DEPTH_SCALE / m->near_z;
    row_map_t still;
    row_map_t own;
    tag_t own_tag = 0;
    row_map(&m->map[0], v, &still);
    float u = 0.5F - m->center_x;
    for (int x = 0; x < width; x++, u += 1.0F) {
        const tag_t t = tag[x];
        if (depth[x] == 0 || t > RASTER_MOTION_INSTANCES_MAX) {
            out[x] = unknown;
            continue;
        }
        if (t != 0 && t != own_tag) {
            row_map(&m->map[t], v, &own);
            own_tag = t;
        }
        out[x] = moved(t == 0 ? &still : &own, u, v, (float)depth[x] * to_w);
    }
}

static void
resolve(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* rows, int index) {
    (void)raster;
    const raster_motion_t* m = self->state;
    for (int y = rows->row0; y < rows->row1; y++) {
        raster_motion_px_t* out = gfx_render_target_row(rows, index, y);
        if (!m->known) {
            for (int x = 0; x < rows->width; x++) {
                out[x] = unknown;
            }
            continue;
        }
        resolve_row(m, gfx_render_target_depth(rows, y), out, rows->width, (float)y + 0.5F - m->center_y);
    }
}

/* Mid-grey for none, a full channel at 16 pixels either way, as 8.8. */
static int32_t
channel(int8_t half_pixels) {
    const int32_t c = 128 + (half_pixels * 4);
    return (c < 0 ? 0 : c > 255 ? 255 : c) << 8;
}

/* Red for x and green for y; unknown pixels take the clear colour, as empty
 * ones do in the depth view. */
static gfx_color_t
pixel_color(const void* pixel, uint16_t clear_color) {
    const raster_motion_px_t* motion = pixel;
    return motion->dx == RASTER_MOTION_UNKNOWN ? clear_color
                                               : r3d_span_pack(channel(motion->dx), channel(motion->dy), 128 << 8);
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* picture, int index) {
    (void)self;
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_motion_view(void* state) {
    return (raster_attachment_t){sizeof(raster_motion_px_t), clear, begin, writer, resolve, show, state};
}

void
raster_motion_forget(raster_motion_t* state) {
    state->has_previous = false;
    state->seen_count = 0;
}
