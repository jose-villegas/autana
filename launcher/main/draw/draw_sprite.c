#include "draw/draw_sprite.h"

#include <assert.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "draw/draw.h"
#include "draw/draw_image.h"
#include "draw/draw_image_format.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "image/pixel/rgb565.h"
#include "math/linear/affine2x.h"
#include "math/scalar/mathi.h"

#define SPRITE_QUARTER_MASK 3
#define SPRITE_COVER_FULL   UINT8_MAX

static asset_status_t
read_border(asset_view_t entry, sprite_t* s) {
    if (entry.size < DRAW_SPRITE_HEADER_SIZE) {
        return ASSET_ERR_BOUNDS;
    }
    const uint8_t* at = entry.data;
    if (asset_read_u16(at) != DRAW_SPRITE_VERSION) {
        return ASSET_ERR_VERSION;
    }
    const uint8_t flat = at[DRAW_SPRITE_AT_FLAT];
    if (flat > 1 || asset_read_u16(at + DRAW_SPRITE_AT_RESERVED) != 0) {
        return ASSET_ERR_FORMAT;
    }
    const uint8_t* border = at + DRAW_SPRITE_AT_BORDER;
    s->border = (sprite_border_t){border[0], border[1], border[2], border[3]};
    s->flat_centre = flat != 0;
    s->centre_alpha = at[DRAW_SPRITE_AT_CENTRE_ALPHA];
    s->centre_color = asset_read_u16(at + DRAW_SPRITE_AT_CENTRE_COLOR);
    return ASSET_OK;
}

static asset_status_t
open_entry(const asset_pack_t* pack, const char* name, sprite_t* out) {
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, name, DRAW_SPRITE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = read_border(entry, out);
        if (status != ASSET_OK) {
            return status;
        }
        entry.data = (const uint8_t*)entry.data + DRAW_SPRITE_HEADER_SIZE;
        entry.size -= DRAW_SPRITE_HEADER_SIZE;
    } else if (status == ASSET_ERR_TYPE) {
        status = asset_pack_find(pack, name, DRAW_IMAGE_ASSET, &entry);
    }
    if (status != ASSET_OK) {
        return status;
    }
    status = draw_image_open_alpha(entry, &out->color, &out->alpha);
    if (status != ASSET_OK) {
        return status;
    }
    if (out->color.format != IMAGE_RGB565BE) {
        return ASSET_ERR_FORMAT;
    }
    const sprite_border_t b = out->border;
    if (b.left + b.right >= out->color.width || b.top + b.bottom >= out->color.height) {
        return ASSET_ERR_BOUNDS;
    }
    return ASSET_OK;
}

asset_status_t
draw_sprite_open(const asset_pack_t* pack, const char* name, sprite_t* out) {
    *out = (sprite_t){0};
    const asset_status_t status = open_entry(pack, name, out);
    if (status != ASSET_OK) {
        *out = (sprite_t){0};
    }
    return status;
}

/* Where the alpha plane starts in resident RAM: after the colour plane, aligned. */
static size_t
alpha_offset(const sprite_t* s) {
    return mathi_size_ceil(image_span_bytes(&s->color), DRAW_IMAGE_PIXEL_ALIGN);
}

size_t
draw_sprite_bytes(const sprite_t* s) {
    return s->alpha.pixels == NULL ? image_span_bytes(&s->color) : alpha_offset(s) + image_span_bytes(&s->alpha);
}

void
draw_sprite_make_resident(sprite_t* s, void* dst) {
    uint8_t* bytes = dst;
    if (s->alpha.pixels != NULL) {
        memcpy(bytes + alpha_offset(s), s->alpha.pixels, image_span_bytes(&s->alpha));
        s->alpha.pixels = bytes + alpha_offset(s);
    }
    memcpy(bytes, s->color.pixels, image_span_bytes(&s->color));
    s->color.pixels = bytes;
}

/* One axis of the nine-slice: the border pair drawn `scale` times its size,
 * shrunk in proportion when the output is shorter than both. */
typedef struct {
    int out_len, src_len, lo, hi, out_lo, out_hi, scale;
} slice_axis_t;

static slice_axis_t
slice_axis(int out_len, int src_len, int lo, int hi, int scale) {
    slice_axis_t axis = {out_len, src_len, lo, hi, lo * scale, hi * scale, scale};
    if (axis.out_lo + axis.out_hi > out_len) {
        axis.out_lo = axis.out_lo * out_len / (axis.out_lo + axis.out_hi);
        axis.out_hi = out_len - axis.out_lo;
    }
    return axis;
}

/* The source pixel along the axis for output pixel `at`: corners `scale`
 * output pixels to a source pixel, the centre stretched by nearest pixel
 * centre. */
static int
slice_source(const slice_axis_t* axis, int at) {
    if (at < axis->out_lo) {
        return at / axis->scale;
    }
    if (at >= axis->out_len - axis->out_hi) {
        return axis->src_len - 1 - ((axis->out_len - 1 - at) / axis->scale);
    }
    const int centre_src = axis->src_len - axis->lo - axis->hi;
    const int centre_out = axis->out_len - axis->out_lo - axis->out_hi;
    return axis->lo + (((2 * (at - axis->out_lo)) + 1) * centre_src / (2 * centre_out));
}

/* One panel row's pixels [x0, x1): source pixel `base + map[col] * step`, the
 * same index in both planes, which share one stride. */
static inline void
blend_span(gfx_color_t* row, int x0, int x1, const int16_t* map, const sprite_t* s, size_t base, size_t step) {
    const gfx_color_t* color = s->color.pixels;
    const uint8_t* alpha = s->alpha.pixels;
    for (int col = x0; col < x1; col++) {
        const size_t at = base + ((size_t)map[col - x0] * step);
        const gfx_color_t pixel = color[at];
        const uint8_t cover = alpha == NULL ? SPRITE_COVER_FULL : alpha[at];
        if (cover == SPRITE_COVER_FULL) {
            row[col] = pixel;
        } else if (cover != 0) {
            row[col] = rgb565be_mix(row[col], pixel, cover);
        }
    }
}

/* The output pixel's source along `axis`, counted from its far end when the
 * turn mirrors that axis. */
static int
axis_source(const slice_axis_t* axis, bool mirror, int at) {
    return slice_source(axis, mirror ? axis->out_len - 1 - at : at);
}

/* A flat centre as one blended rectangle; the panel rectangle it covers, or
 * an empty one for a sprite without one. */
static recti_t
draw_flat_centre(const sprite_t* s, recti_t dst, int turns, const slice_axis_t* sx, const slice_axis_t* sy) {
    if (!s->flat_centre) {
        return recti_empty();
    }
    const recti_t inner = {sx->out_lo, sy->out_lo, sx->out_len - sx->out_lo - sx->out_hi,
                           sy->out_len - sy->out_lo - sy->out_hi};
    const affine2x_t turn = affine2x_quarter_turn(turns, dst.w, dst.h);
    recti_t centre = affine2x_map_recti(&turn, inner);
    centre.x += dst.x;
    centre.y += dst.y;
    draw_rect_blend(centre.x, centre.y, centre.w, centre.h, s->centre_color, s->centre_alpha);
    return centre;
}

/* Panel row `row` across `rect`, leaving the columns `centre` already drew. */
static void
blend_row(gfx_color_t* line, int row, recti_t rect, recti_t centre, const int16_t* map, const sprite_t* s, size_t base,
          size_t step) {
    const int x1 = recti_right(rect);
    const int skip0 = mathi_max(centre.x, rect.x);
    const int skip1 = mathi_min(recti_right(centre), x1);
    if (row >= centre.y && row < recti_bottom(centre) && skip0 < skip1) {
        blend_span(line, rect.x, skip0, map, s, base, step);
        blend_span(line, skip1, x1, map + (skip1 - rect.x), s, base, step);
        return;
    }
    blend_span(line, rect.x, x1, map, s, base, step);
}

/* The clipped panel rectangle `rect` of a sprite filling `dst` at `turns`:
 * panel columns walk `col_axis` and panel rows `row_axis`. */
static void
blit(const gfx_target_t* target, recti_t rect, recti_t dst, const sprite_t* s, int turns, const slice_axis_t* col_axis,
     const slice_axis_t* row_axis, recti_t centre) {
    const bool col_mirror = turns == 1 || turns == 2;
    const bool row_mirror = turns == 2 || turns == 3;
    int16_t map[GFX_WIDTH];
    for (int col = rect.x; col < recti_right(rect); col++) {
        map[col - rect.x] = (int16_t)axis_source(col_axis, col_mirror, col - dst.x);
    }
    /* Upright, a panel row is a sprite row; turned, a sprite column. */
    const bool across = (turns & 1) != 0;
    const size_t stride = s->color.pitch / sizeof(gfx_color_t);
    const size_t step = across ? stride : 1;
    const size_t row_step = across ? 1 : stride;
    for (int row = rect.y; row < recti_bottom(rect); row++) {
        const size_t base = (size_t)axis_source(row_axis, row_mirror, row - dst.y) * row_step;
        blend_row(gfx_target_color(target, row), row, rect, centre, map, s, base, step);
    }
}

void
draw_sprite(const sprite_t* s, recti_t dst, int quarter_turns) {
    draw_sprite_scaled(s, dst, quarter_turns, 1);
}

void
draw_sprite_scaled(const sprite_t* s, recti_t dst, int quarter_turns, int scale) {
    assert(scale >= 1);
    const int turns = quarter_turns & SPRITE_QUARTER_MASK;
    const bool across = (turns & 1) != 0; /* the sprite's x runs down the panel */
    const recti_t frame = across ? (recti_t){0, 0, dst.h, dst.w} : (recti_t){0, 0, dst.w, dst.h};
    if (recti_is_empty(frame)) {
        return;
    }
    const slice_axis_t sx = slice_axis(frame.w, s->color.width, s->border.left, s->border.right, scale);
    const slice_axis_t sy = slice_axis(frame.h, s->color.height, s->border.top, s->border.bottom, scale);
    const recti_t centre = draw_flat_centre(s, dst, turns, &sx, &sy);

    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    const recti_t rect = gfx_target_clip_rect(state.target, state.clip, dst);
    if (recti_is_empty(rect)) {
        return;
    }
    assert(rect.w <= GFX_WIDTH); /* every target is the panel's width */
    blit(state.target, rect, dst, s, turns, across ? &sy : &sx, across ? &sx : &sy, centre);
    if (state.marks) {
        gfx_mark_drawn(rect.x, rect.y, rect.w, rect.h);
    }
}
