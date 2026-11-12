#include "draw/draw_sprite.h"

#include <string.h>

#include "asset/asset_bytes.h"
#include "draw/draw.h"
#include "draw/draw_image.h"
#include "draw/draw_image_format.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "image/pixel/rgb565.h"

/* The longest run of panel columns one draw maps: the panel's longer side. */
#define SPRITE_SPAN_MAX     (GFX_WIDTH > GFX_HEIGHT ? GFX_WIDTH : GFX_HEIGHT)
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

/* A plane's bytes up to the end of its last pixel: a pack's last row may stop there. */
static size_t
plane_bytes(const image_t* plane) {
    if (plane->pixels == NULL) {
        return 0;
    }
    return ((size_t)plane->pitch * (plane->height - 1U)) + IMAGE_PITCH(plane->format, plane->width);
}

/* Where the alpha plane starts in resident RAM: after the colour plane, aligned. */
static size_t
alpha_offset(const sprite_t* s) {
    const size_t align = DRAW_IMAGE_PIXEL_ALIGN;
    return (plane_bytes(&s->color) + align - 1U) / align * align;
}

size_t
draw_sprite_bytes(const sprite_t* s) {
    return s->alpha.pixels == NULL ? plane_bytes(&s->color) : alpha_offset(s) + plane_bytes(&s->alpha);
}

void
draw_sprite_make_resident(sprite_t* s, void* dst) {
    uint8_t* bytes = dst;
    if (s->alpha.pixels != NULL) {
        memcpy(bytes + alpha_offset(s), s->alpha.pixels, plane_bytes(&s->alpha));
        s->alpha.pixels = bytes + alpha_offset(s);
    }
    memcpy(bytes, s->color.pixels, plane_bytes(&s->color));
    s->color.pixels = bytes;
}

/* One axis of the nine-slice: the border pair shrunk in proportion when the
 * output is shorter than both. */
typedef struct {
    int out_len, src_len, lo, hi, out_lo, out_hi;
} slice_axis_t;

static slice_axis_t
slice_axis(int out_len, int src_len, int lo, int hi) {
    slice_axis_t axis = {out_len, src_len, lo, hi, lo, hi};
    if (lo + hi > out_len) {
        axis.out_lo = lo * out_len / (lo + hi);
        axis.out_hi = out_len - axis.out_lo;
    }
    return axis;
}

/* The source pixel along the axis for output pixel `at`: corners 1:1, the
 * centre stretched by nearest pixel centre. */
static int
slice_source(const slice_axis_t* axis, int at) {
    if (at < axis->out_lo) {
        return at;
    }
    if (at >= axis->out_len - axis->out_hi) {
        return axis->src_len - (axis->out_len - at);
    }
    const int centre_src = axis->src_len - axis->lo - axis->hi;
    const int centre_out = axis->out_len - axis->out_lo - axis->out_hi;
    return axis->lo + (((2 * (at - axis->out_lo)) + 1) * centre_src / (2 * centre_out));
}

/* The panel rectangle a sprite-frame rectangle `r` lands on, in a `w` x `h`
 * sprite frame turned clockwise by `turns`, placed at `at`. */
static recti_t
turned_rect(recti_t r, int w, int h, int turns, recti_t at) {
    recti_t out;
    switch (turns) {
        case 1: out = (recti_t){h - recti_bottom(r), r.x, r.h, r.w}; break;
        case 2: out = (recti_t){w - recti_right(r), h - recti_bottom(r), r.w, r.h}; break;
        case 3: out = (recti_t){r.y, w - recti_right(r), r.h, r.w}; break;
        default: out = r; break;
    }
    out.x += at.x;
    out.y += at.y;
    return out;
}

/* One panel row's pixels [x0, x1): source index `base + map[col] * step`. */
static inline void
blend_span(gfx_color_t* row, int x0, int x1, const int16_t* map, const gfx_color_t* color, const uint8_t* alpha,
           size_t color_base, size_t color_step, size_t alpha_base, size_t alpha_step) {
    for (int col = x0; col < x1; col++) {
        const size_t m = (size_t)map[col - x0];
        const gfx_color_t pixel = color[color_base + (m * color_step)];
        const uint8_t cover = alpha == NULL ? SPRITE_COVER_FULL : alpha[alpha_base + (m * alpha_step)];
        if (cover == SPRITE_COVER_FULL) {
            row[col] = pixel;
        } else if (cover != 0) {
            row[col] = rgb565be_mix(row[col], pixel, cover);
        }
    }
}

void
draw_sprite(const sprite_t* s, recti_t dst, int quarter_turns) {
    const int turns = quarter_turns & SPRITE_QUARTER_MASK;
    const bool across = (turns & 1) != 0; /* the sprite's x runs down the panel */
    const int w = across ? dst.h : dst.w;
    const int h = across ? dst.w : dst.h;
    if (w <= 0 || h <= 0) {
        return;
    }
    const sprite_border_t b = s->border;
    const slice_axis_t sx = slice_axis(w, s->color.width, b.left, b.right);
    const slice_axis_t sy = slice_axis(h, s->color.height, b.top, b.bottom);

    recti_t centre = recti_empty();
    if (s->flat_centre) {
        const recti_t inner = {sx.out_lo, sy.out_lo, w - sx.out_lo - sx.out_hi, h - sy.out_lo - sy.out_hi};
        centre = turned_rect(inner, w, h, turns, dst);
        draw_rect_blend(centre.x, centre.y, centre.w, centre.h, s->centre_color, s->centre_alpha);
    }

    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    const recti_t rect = gfx_target_clip_rect(state.target, state.clip, dst);
    if (recti_is_empty(rect) || rect.w > SPRITE_SPAN_MAX) {
        return;
    }

    /* Panel columns map to one sprite axis and panel rows to the other; a
     * mirrored axis counts from its far end. */
    const slice_axis_t* col_axis = across ? &sy : &sx;
    const slice_axis_t* row_axis = across ? &sx : &sy;
    const bool col_mirror = turns == 1 || turns == 2;
    const bool row_mirror = turns == 2 || turns == 3;
    int16_t map[SPRITE_SPAN_MAX];
    for (int col = rect.x; col < recti_right(rect); col++) {
        const int at = col - dst.x;
        map[col - rect.x] = (int16_t)slice_source(col_axis, col_mirror ? col_axis->out_len - 1 - at : at);
    }

    const gfx_color_t* color = s->color.pixels;
    const uint8_t* alpha = s->alpha.pixels;
    const size_t color_pitch = s->color.pitch / sizeof(gfx_color_t);
    const size_t alpha_pitch = s->alpha.pitch;
    for (int row = rect.y; row < recti_bottom(rect); row++) {
        const int at = row - dst.y;
        const size_t fixed = (size_t)slice_source(row_axis, row_mirror ? row_axis->out_len - 1 - at : at);
        /* Upright, a panel row is a sprite row; turned, a sprite column. */
        const size_t color_base = across ? fixed : fixed * color_pitch;
        const size_t alpha_base = across ? fixed : fixed * alpha_pitch;
        const size_t color_step = across ? color_pitch : 1;
        const size_t alpha_step = across ? alpha_pitch : 1;
        gfx_color_t* line = gfx_target_color(state.target, row);
        int x0 = rect.x;
        int x1 = recti_right(rect);
        if (s->flat_centre && row >= centre.y && row < recti_bottom(centre)) {
            const int skip0 = centre.x > x0 ? centre.x : x0;
            const int skip1 = recti_right(centre) < x1 ? recti_right(centre) : x1;
            if (skip0 < skip1) {
                blend_span(line, x0, skip0, map, color, alpha, color_base, color_step, alpha_base, alpha_step);
                blend_span(line, skip1, x1, map + (skip1 - x0), color, alpha, color_base, color_step, alpha_base,
                           alpha_step);
                continue;
            }
        }
        blend_span(line, x0, x1, map, color, alpha, color_base, color_step, alpha_base, alpha_step);
    }

    if (state.marks) {
        gfx_mark_drawn(rect.x, rect.y, rect.w, rect.h);
    }
}
