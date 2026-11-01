#include "render/view/raster_motion.h"
#include "core/checkpoint.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "core/memory.h"
#include "math/scalar/mathf.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"
#include "render/view/raster_show.h"

/* While drawing, the attachment holds each pixel's tag; resolving turns the
 * tag and the depth into the motion in place. */
typedef raster_motion_tag_t tag_t;
_Static_assert(sizeof(tag_t) == sizeof(raster_motion_px_t), "a tag resolves in place");
_Static_assert(RASTER_MOTION_INSTANCES_MAX == (tag_t)-1, "instance i tags i + 1");

#define DEPTH_SCALE (1.0F / (float)R3D_DEPTH_NEAREST)

/* World to lens space for `view`, the raster's own lens at one tick to the
 * unit, then carried by `model` when it is not NULL, as r3d_lens_place()
 * does for a placed mesh. */
static void
lens_map(const render_view_t* view, const raster_t* raster, const mat4f_t* model, r3d_lens_t* lens) {
    raster_lens(raster, view, 1, lens);
    if (model != NULL) {
        r3d_lens_place(lens, model, 1);
    }
}

/* Where `key` was seen, trying `slot` first: the order rarely changes
 * between pictures. */
static int
seen_index(const raster_motion_t* m, const mat4f_t* key, int slot) {
    if (slot < m->seen_count && m->seen_key[slot] == key) {
        return slot;
    }
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
        const mat4f_t* p = raster->instances[i].model;
        const int seen = p == NULL ? -1 : seen_index(m, p, i);
        if (seen >= 0 && memcmp(&m->seen[seen], p, sizeof(*p)) != 0) {
            lens_map(&m->previous, raster, &m->seen[seen], &m->before);
            lens_map(view, raster, p, &m->now);
            m->map[i + 1] = mat4f_mul_affine(m->before.m, mat4f_invert_affine(m->now.m));
            m->moved[i] = true;
            m->first_moved = m->first_moved < 0 ? i : m->first_moved;
        }
    }
}

/* This picture's view and models, for the next one. */
static void
remember(raster_motion_t* m, const raster_t* raster, const render_view_t* view) {
    m->previous = *view;
    m->has_previous = true;
    m->seen_count = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        const mat4f_t* p = raster->instances[i].model;
        if (p != NULL) {
            m->seen_key[m->seen_count] = p;
            m->seen[m->seen_count++] = *p;
        }
    }
}

/* The maps for this picture, then this picture kept as the next one's
 * previous. A model is known by its address from picture to picture. */
static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    raster_motion_t* m = self->state;
    assert(raster->instance_count <= m->capacity); /* raster_motion_reserve() first */
    lens_map(view, raster, NULL, &m->now);
    m->project = m->now.project;
    m->known = m->has_previous;
    m->first_moved = -1;
    memset(m->moved, 0, (size_t)raster->instance_count * sizeof(*m->moved));
    if (m->known) {
        render_view_refit(&m->previous, view->viewport);
        lens_map(&m->previous, raster, NULL, &m->before);
        m->map[0] = mat4f_mul_affine(m->before.m, mat4f_invert_affine(m->now.m));
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
 * with inverse depth iz is the point (u, v, 1) depth_near / iz. Through a map
 * (A, t) it lands at (A (u, v, 1) + t iz / depth_near) depth_near / iz, whose
 * screen position is the first two over the third; w is iz / depth_near,
 * with depth_near the picture's own (raster_depth_near()). */
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
resolve_row(const raster_motion_t* m, const uint16_t* depth, float depth_near, raster_motion_px_t* out, int width,
            float v) {
    const tag_t* tag = (const tag_t*)out;
    const float to_w = DEPTH_SCALE / depth_near;
    row_map_t still;
    row_map_t own;
    tag_t own_tag = 0;
    const int tags = m->capacity; /* out's stores may alias m: read once */
    row_map(&m->map[0], v, &still);
    float u = 0.5F - m->project.cx;
    for (int x = 0; x < width; x++, u += 1.0F) {
        const tag_t t = tag[x];
        if (depth[x] == 0 || t > tags) {
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
resolve(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* rows, int index) {
    const raster_motion_t* m = self->state;
    const float depth_near = raster_depth_near(raster);
    for (int y = rows->first_row; y < gfx_target_end_row(rows); y++) {
        raster_motion_px_t* out = gfx_target_row(rows, index, y);
        if (!m->known) {
            for (int x = 0; x < rows->attachment[0].width; x++) {
                out[x] = unknown;
            }
            continue;
        }
        resolve_row(m, gfx_target_depth(rows, y), depth_near, out, rows->attachment[0].width,
                    (float)y + 0.5F - m->project.cy);
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
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    (void)self;
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_motion_view(void* state) {
    return (raster_attachment_t){IMAGE_U16, clear, begin, writer, resolve, show, state};
}

void
raster_motion_forget(raster_motion_t* state) {
    state->has_previous = false;
    state->seen_count = 0;
}

/* The arrays in one block, the widest first. */
static size_t
block_bytes(int capacity) {
    const size_t n = (size_t)capacity;
    return ((n + 1) * sizeof(mat4f_t)) + (n * sizeof(mat4f_t)) + (n * sizeof(const mat4f_t*)) + (n * sizeof(bool));
}

bool
raster_motion_reserve(raster_motion_t* m, int instances) {
    if (instances <= m->capacity) {
        return true;
    }
    if (instances > RASTER_MOTION_INSTANCES_MAX) {
        return false;
    }
    uint8_t* block = memory_alloc(block_bytes(instances), MEMORY_PSRAM);
    if (block == NULL) {
        return false;
    }
    const size_t n = (size_t)instances;
    mat4f_t* map = (mat4f_t*)block;
    mat4f_t* seen = map + n + 1;
    const mat4f_t** seen_key = (const mat4f_t**)(seen + n);
    bool* moved = (bool*)(seen_key + n);
    if (m->seen_count > 0) {
        memcpy(seen, m->seen, (size_t)m->seen_count * sizeof(*seen));
        memcpy(seen_key, m->seen_key, (size_t)m->seen_count * sizeof(*seen_key));
    }
    memory_free(m->block); /* outgrown: its history is copied */
    m->block = block;
    m->capacity = instances;
    m->map = map;
    m->seen = seen;
    m->seen_key = seen_key;
    m->moved = moved;
    return true;
}

void
raster_motion_release(raster_motion_t* m) {
    memory_free(m->block);
    *m = (raster_motion_t){0};
}

bool
raster_motion_reserve_view(void* state, int instances) {
    return raster_motion_reserve(state, instances);
}

void
raster_motion_release_view(void* state) {
    raster_motion_release(state);
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
raster_motion_checkpoint(checkpoint_t* saved, void* state) {
    raster_motion_t* motion = state;
    checkpoint_capture_at(saved, &motion->block, block_bytes(motion->capacity));
    checkpoint_capture(saved, motion, sizeof(*motion));
}
#endif
