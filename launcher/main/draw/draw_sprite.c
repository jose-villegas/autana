#include "draw/draw_sprite.h"

#include <assert.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "draw/draw.h"
#include "draw/draw_blend.h"
#include "draw/draw_image.h"
#include "draw/draw_image_format.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "image/pixel/rgb565.h"
#include "math/linear/affine2x.h"
#include "math/scalar/mathi.h"
#include "simd/fill.h"

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

/* The slices of an axis, in output order; SLICE_NONE is no slice yet. */
typedef enum { SLICE_NONE = -1, SLICE_LO, SLICE_CENTRE, SLICE_HI } slice_t;

static slice_t
slice_of(const slice_axis_t* axis, int at) {
    if (at < axis->out_lo) {
        return SLICE_LO;
    }
    return at < axis->out_len - axis->out_hi ? SLICE_CENTRE : SLICE_HI;
}

/* Output pixel `at` along the axis, counted from its far end when the turn
 * mirrors it. */
static int
axis_at(const slice_axis_t* axis, bool mirror, int at) {
    return mirror ? axis->out_len - 1 - at : at;
}

/* The source pixel for output pixel `at` in `slice`: corners `scale` output
 * pixels to a source pixel, the centre stretched by nearest pixel centre. */
static int
slice_source(const slice_axis_t* axis, slice_t slice, int at) {
    if (slice == SLICE_LO) {
        return at / axis->scale;
    }
    if (slice == SLICE_HI) {
        return axis->src_len - 1 - ((axis->out_len - 1 - at) / axis->scale);
    }
    const int centre_src = axis->src_len - axis->lo - axis->hi;
    const int centre_out = axis->out_len - axis->out_lo - axis->out_hi;
    return axis->lo + (((2 * (at - axis->out_lo)) + 1) * centre_src / (2 * centre_out));
}

/* The source pixel along `axis` for output pixel `at`, mirrored or not. */
static int
axis_source(const slice_axis_t* axis, bool mirror, int at) {
    const int along = axis_at(axis, mirror, at);
    return slice_source(axis, slice_of(axis, along), along);
}

/* `count` panel pixels that all show one source pixel: a stretched edge
 * repeats one, so its run blends toward one colour at one share. */
static inline __attribute__((always_inline)) void
blend_run(gfx_color_t* dst, int count, gfx_color_t pixel, uint8_t cover) {
    if (cover == 0) {
        return;
    }
    if (cover == SPRITE_COVER_FULL) {
        simd_fill_u16(dst, pixel, (size_t)count);
    } else if (count == 1) {
        dst[0] = rgb565be_mix(dst[0], pixel, cover);
    } else {
        const rgb565_mix_toward_t toward = rgb565be_mix_toward(pixel, cover);
        draw_blend_row(dst, &toward, (size_t)count);
    }
}

/* Output pixels from `at` to the end of border slice `slice`, walking up the
 * axis, or down it when `down`. */
static int
border_left(const slice_axis_t* axis, slice_t slice, int at, bool down) {
    if (down) {
        return slice == SLICE_LO ? at + 1 : at - (axis->out_len - axis->out_hi) + 1;
    }
    return (slice == SLICE_LO ? axis->out_lo : axis->out_len) - at;
}

/* How many output pixels from `at` on, in `slice`, show the source pixel `at`
 * shows, walking up the axis, or down it when `down`: `scale` a border pixel,
 * or a stretched centre pixel's span, ending where its slice does. */
static int
slice_run(const slice_axis_t* axis, slice_t slice, int at, bool down) {
    const int centre_at = axis->out_lo;
    const int hi_at = axis->out_len - axis->out_hi;
    if (slice == SLICE_LO) {
        const int first = (at / axis->scale) * axis->scale;
        return down ? at - first + 1 : mathi_min(first + axis->scale, centre_at) - at;
    }
    if (slice == SLICE_HI) {
        const int from_end = axis->out_len - 1 - at;
        const int last = axis->out_len - 1 - ((from_end / axis->scale) * axis->scale);
        return down ? at - mathi_max(last - axis->scale + 1, hi_at) + 1 : last - at + 1;
    }
    /* Centre output pixel k shows lo + q, q = (2k + 1) * centre_src / (2 *
     * centre_out): the k showing q start at ceil(q * 2 * centre_out /
     * centre_src) / 2, so a run is found with two divides, not one a pixel. */
    const int centre_src = axis->src_len - axis->lo - axis->hi;
    const int twice_out = 2 * (hi_at - centre_at);
    const int k = at - centre_at;
    const int q = ((2 * k) + 1) * centre_src / twice_out;
    if (down) {
        return k - (mathi_ceil_div(q * twice_out, centre_src) / 2) + 1;
    }
    return mathi_min(mathi_ceil_div((q + 1) * twice_out, centre_src) / 2, hi_at - centre_at) - k;
}

/* A walk along an axis: the slice of its last run and that run's source
 * pixel. */
typedef struct {
    slice_t slice;
    int src;
} axis_walk_t;

/* The run from output pixel `at`, the next after the walk's last: its length,
 * its source pixel left in the walk. Past its first run, a border slice steps
 * a source pixel each `scale` output pixels, so only a slice's first run
 * divides. */
static inline __attribute__((always_inline)) int
walk_run(axis_walk_t* walk, const slice_axis_t* axis, int at, bool down) {
    const slice_t in = slice_of(axis, at);
    if (in == walk->slice && in != SLICE_CENTRE) {
        walk->src += down ? -1 : 1;
        return mathi_min(axis->scale, border_left(axis, in, at, down));
    }
    walk->slice = in;
    walk->src = slice_source(axis, in, at);
    return slice_run(axis, in, at, down);
}

/* One panel row's pixels [x0, x1) of a sprite whose column axis is `axis`
 * from panel column `origin`, mirrored or not: its source pixels are `base +
 * source * step`, the same index in both planes, which share one stride.
 * Each source pixel is read once, for the run of panel pixels that shows
 * it; neighbouring runs of one colour and share blend as one, across slice
 * edges, so a row of one colour from border to border blends once. */
static inline __attribute__((always_inline)) void
blend_span(gfx_color_t* row, int x0, int x1, const slice_axis_t* axis, bool mirror, int origin, const sprite_t* s,
           size_t base, size_t step) {
    const gfx_color_t* color = s->color.pixels;
    const uint8_t* alpha = s->alpha.pixels;
    int start = x0;
    gfx_color_t pixel = 0;
    uint8_t cover = 0;
    axis_walk_t walk = {SLICE_NONE, 0};
    for (int col = x0; col < x1;) {
        const int at = axis_at(axis, mirror, col - origin);
        const int count = mathi_min(walk_run(&walk, axis, at, mirror), x1 - col);
        const size_t source = base + ((size_t)walk.src * step);
        const gfx_color_t next = color[source];
        const uint8_t next_cover = alpha == NULL ? SPRITE_COVER_FULL : alpha[source];
        if (col > start && (next != pixel || next_cover != cover)) {
            blend_run(row + start, col - start, pixel, cover);
            start = col;
        }
        pixel = next;
        cover = next_cover;
        col += count;
    }
    if (x1 > start) {
        blend_run(row + start, x1 - start, pixel, cover);
    }
}

/* A flat centre as one blended rectangle; the panel rectangle it covers, or
 * an empty one for a sprite without one. */
static __attribute__((noinline)) recti_t
flat_centre_rect(const sprite_t* s, recti_t dst, int turns, const slice_axis_t* sx, const slice_axis_t* sy) {
    if (!s->flat_centre) {
        return recti_empty();
    }
    const recti_t inner = {sx->out_lo, sy->out_lo, sx->out_len - sx->out_lo - sx->out_hi,
                           sy->out_len - sy->out_lo - sy->out_hi};
    const affine2x_t turn = affine2x_quarter_turn(turns, dst.w, dst.h);
    recti_t centre = affine2x_map_recti(&turn, inner);
    centre.x += dst.x;
    centre.y += dst.y;
    return centre;
}

/* The columns of a sprite row: its axis, whether a turn mirrors it, the
 * panel column it starts at, and the step between its source pixels. */
typedef struct {
    const slice_axis_t* axis;
    bool mirror;
    int origin;
    size_t step;
} span_axis_t;

/* Panel row `row` across `rect`, leaving the columns `centre` already drew:
 * the sprite row at `base` along `cols`. */
static void
blend_row(gfx_color_t* line, int row, recti_t rect, recti_t centre, const span_axis_t* cols, const sprite_t* s,
          size_t base) {
    const int x1 = recti_right(rect);
    const int skip0 = mathi_max(centre.x, rect.x);
    const int skip1 = mathi_min(recti_right(centre), x1);
    if (row >= centre.y && row < recti_bottom(centre) && skip0 < skip1) {
        blend_span(line, rect.x, skip0, cols->axis, cols->mirror, cols->origin, s, base, cols->step);
        blend_span(line, skip1, x1, cols->axis, cols->mirror, cols->origin, s, base, cols->step);
        return;
    }
    blend_span(line, rect.x, x1, cols->axis, cols->mirror, cols->origin, s, base, cols->step);
}

/* The clipped panel rectangle `clipped` of a sprite filling `dst` at `turns`:
 * panel columns walk `col_axis` and panel rows `row_axis`. Out of line, so
 * its locals are off the stack while the flat centre draws: a draw stays
 * shallow under a caller already deep in its own stack. */
static __attribute__((noinline)) void
blit(const gfx_target_t* target, const recti_t* clipped, const recti_t* dst, const sprite_t* s, int turns,
     const slice_axis_t* col_axis, const slice_axis_t* row_axis, const recti_t* centre) {
    const recti_t rect = *clipped;
    const bool row_mirror = turns == 2 || turns == 3;
    /* Upright, a panel row is a sprite row; turned, a sprite column. */
    const bool across = (turns & 1) != 0;
    const size_t stride = s->color.pitch / sizeof(gfx_color_t);
    const size_t row_step = across ? 1 : stride;
    const span_axis_t cols = {col_axis, turns == 1 || turns == 2, dst->x, across ? stride : 1};
    for (int row = rect.y; row < recti_bottom(rect); row++) {
        const size_t base = (size_t)axis_source(row_axis, row_mirror, row - dst->y) * row_step;
        blend_row(gfx_target_color(target, row), row, rect, *centre, &cols, s, base);
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
    const recti_t centre = flat_centre_rect(s, dst, turns, &sx, &sy);
    if (s->flat_centre) {
        draw_rect_blend(centre.x, centre.y, centre.w, centre.h, s->centre_color, s->centre_alpha);
    }

    gfx_surface_t state;
    if (!gfx_draw_begin(&state)) {
        return;
    }
    const recti_t rect = gfx_target_clip_rect(state.target, state.clip, dst);
    if (recti_is_empty(rect)) {
        return;
    }
    blit(state.target, &rect, &dst, s, turns, across ? &sy : &sx, across ? &sx : &sy, &centre);
    if (state.marks) {
        gfx_mark_drawn(rect.x, rect.y, rect.w, rect.h);
    }
}
