/*
 * ui - paints one microui frame into the real framebuffer. See ui.h for what
 * and why, and ui_build.c for the other half of this module: building the
 * frame this file paints, split out specifically because it needs none of
 * gfx.c and so can run on a host - see ui_build.c's own top comment.
 *
 * THE CANVAS MODEL
 *
 * A retained-mode engine knows what changed because changing it is an explicit
 * act: you mutate a node, the node marks itself dirty, and only its canvas is
 * rebuilt. Immediate mode throws that signal away by construction - the UI is
 * rebuilt from scratch every frame, so "was it modified?" has no answer.
 *
 * The signal is recoverable from the other end. microui's command list is a
 * complete description of the output, so two frames that hash the same ARE the
 * same picture. Comparing output where an engine compares intent gets to the
 * same place by a different route.
 *
 * The canvas split comes free with it. microui already groups commands by root
 * container, each with its own rect, so ONE WINDOW IS ONE CANVAS: hashed on its
 * own, repainted on its own, and marking only its own bands dirty. A live
 * readout in one window therefore does not force a static toolbar in another to
 * repaint, which is the whole point of splitting canvases in the first place.
 *
 * The one rule that has to be respected is painter's order. Windows are drawn
 * back to front, so repainting one means repainting anything above it that
 * overlaps - otherwise the repaint erases what was on top.
 */

#include "ui/ui.h"

#include "esp_log.h"

#include "build_variant.h"
#include "gfx/gfx.h"
#include "gfx/gfx_target.h"
#include "gfx/icons_system.h"
#include "ui/ui_internal.h"

static const char* TAG = "ui";

#if CONFIG_LAUNCHER_DEVELOPMENT
/* MU_COMMANDLIST_SIZE (8 KiB, microui.h) was sized against an estimate, not
 * a measurement - this makes it one. Logs only on a new high, so a screen
 * that has already shown its worst frame costs nothing more to watch. */
static int command_list_high_water;

static void
report_command_list_high_water(int used) {
    if (used > command_list_high_water) {
        command_list_high_water = used;
        ESP_LOGI(TAG, "command list high water: %d / %d bytes", used, MU_COMMANDLIST_SIZE);
    }
}
#endif

/*
 * Painting
 *
 * Every command's geometry is mapped through the transform in force before
 * it reaches gfx - see ui_transform.h for what that buys, and ui_build.c's
 * ui_set_transform() for why an invalid one renders as identity rather than
 * being rejected at the point it was set.
 */

typedef struct {
    gfx_color_t color;
} icon_fill_ctx_t;

static void
icon_fill_emit(void* ctx_, int x, int y, int w, int h) {
    const icon_fill_ctx_t* fc = ctx_;
    gfx_fill_rect(x, y, w, h, fc->color);
}

/* microui colour to the panel's own, alpha dropped - a style works in
 * microui's colour space and never sees a panel pixel; this is the one
 * place a command's colour crosses into gfx's. */
static gfx_color_t
mu_color_to_gfx(mu_Color c) {
    return gfx_rgb(((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b);
}

/* The inverse, opaque - for a caller that only has a packed 0xRRGGBB and
 * needs an mu_Color to hand to a fill helper. */
static mu_Color
mu_color_from_rgb(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

static void
draw_text_command(const mu_Command* cmd, ui_transform_t t) {
    const mu_Color ink = cmd->text.color;
    const mu_Color halo = ui_text_halo(ink);
    const ui_font_scaled_t fs = ui_resolve_font_scaled(cmd->text.font);
    const gfx_font_t* font = fs.font;
    const int scale = fs.scale;
    const int quarter = ui_transform_quarter(t);

    /* WHY THE WHOLE STRING'S BOX IS MAPPED, NOT ITS ORIGIN: every
     * other command maps its rect through ui_transform_rect(), proven
     * exact under any quarter turn. A point does not commute with
     * "walk N glyphs, take the far edge" under rotation, so mapping
     * just the origin and walking per-glyph from there drifts a
     * string off at quarter turns 1-3. Mapping the LOGICAL box
     * instead - the same one used to size it - keeps it exact, like
     * every other command. */
    const int tw = gfx_font_text_width(font, cmd->text.str, -1, scale);
    const int th = gfx_font_height(font, scale);
    const mu_Rect box = ui_transform_rect(t, (mu_Rect){cmd->text.pos.x, cmd->text.pos.y, tw, th});

    /* gfx_text_font()'s (x, y) is the FIRST GLYPH's cell, not a
     * corner of the box - see ui_text_glyph0_origin()'s own comment
     * (ui_transform.h) for the full derivation. Extracted there, not
     * kept inline, so it's testable against a synthetic proportional
     * font on a host - a port of the same origin math an app's own
     * label-drawing code solves for itself, ported rather than called
     * directly because ui/ sits below apps/, so reaching into an
     * app's source would be a backwards layering dependency. */
    int mx, my;
    ui_text_glyph0_origin(font, box, quarter, scale, &mx, &my);

    /* A text colour's alpha is dithered coverage. No halo has a
     * dithered form, so a fading string is drawn as ink alone. */
    if (ink.a < 255) {
        if (ink.a > 0) {
            gfx_text_font_dither(mx, my, cmd->text.str, mu_color_to_gfx(ink), scale, quarter, font, ink.a);
        }
        return;
    }

    if (ui_text_style == UI_TEXT_OUTLINED && font->bpp == 1) {
        /* gfx_text_font_halo() draws the same halo ui_text_passes()'s
         * 8 unit-offset copies would, in one pass instead of eight -
         * see its own comment. Ink is still drawn last, unchanged. */
        const gfx_color_t halo_color = mu_color_to_gfx(halo);
        const gfx_color_t ink_color = mu_color_to_gfx(ink);
        gfx_text_font_halo(mx, my, cmd->text.str, halo_color, scale, quarter, font);
        gfx_text_font(mx, my, cmd->text.str, ink_color, scale, quarter, font);
        return;
    }

    ui_text_pass_t passes[UI_TEXT_MAX_PASSES];
    const int n = ui_text_passes(ui_text_style, passes, UI_TEXT_MAX_PASSES);
    for (int i = 0; i < n; i++) {
        const mu_Color c = passes[i].ink ? ink : halo;
        const gfx_color_t color = mu_color_to_gfx(c);

        /* THE HALO OFFSET IS ADDED AFTER THE MAPPING, NOT BEFORE.
         * passes[i].dx/dy is a SCREEN-SPACE offset - see ui_style.h's
         * OUTLINED/SHADOWED passes, which sit a halo a fixed pixel
         * count from the glyph on the panel. (mx, my) already IS a
         * screen position. Transforming (dx, dy) itself would instead
         * rotate the halo with the glyph: a shadow meant to fall
         * down-and-right on screen would fall down-and-right in
         * LOGICAL space instead - a different physical direction once
         * turn is nonzero. */
        gfx_text_font(mx + passes[i].dx, my + passes[i].dy, cmd->text.str, color, scale, quarter, font);
    }
}

static void
draw_command(const mu_Command* cmd) {
    const ui_transform_t t = ui_effective_transform();

    switch (cmd->type) {

        case MU_COMMAND_RECT: {
            const mu_Color c = cmd->rect.color;
            /* Fully transparent rects are microui's way of drawing nothing; we
         * have no blending, so skip them rather than paint black. */
            if (c.a == 0) {
                break;
            }
            const mu_Rect r = ui_transform_rect(t, cmd->rect.rect);
            gfx_fill_rect(r.x, r.y, r.w, r.h, mu_color_to_gfx(c));
            break;
        }

        case MU_COMMAND_TEXT: draw_text_command(cmd, t); break;

        case MU_COMMAND_ICON: {
            /* microui's icons are close/check/collapsed/expanded. MU_ICON_CHECK
         * is real artwork (gfx/icons_system.h's baked ICON_SYSTEM_CHECK)
         * because two callers need it: a checkbox toggle, and a per-tile
         * spawn-selection badge. The other three stay a small
         * centred-square placeholder - a deliberate gap, not
         * an oversight, because nothing in this shell closes a window or
         * collapses a tree yet to ask for them. */
            const mu_Color c = cmd->icon.color;
            const gfx_color_t color = mu_color_to_gfx(c);
            if (cmd->icon.id == MU_ICON_CHECK) {
                const icon_t* icon = &icon_system_table[ICON_SYSTEM_CHECK];
                icon_fill_ctx_t fc = {.color = color};
                ui_transform_icon_blocks(t, icon_system_rows + icon->offset, icon->w, icon->h, icon->stride,
                                         cmd->icon.rect, icon_fill_emit, &fc);
            } else {
                const mu_Rect r = ui_transform_rect(t, cmd->icon.rect);
                gfx_fill_rect(r.x + r.w / 3, r.y + r.h / 3, r.w / 3, r.h / 3, color);
            }
            break;
        }

        case MU_COMMAND_CLIP: {
            const mu_Rect r = ui_transform_rect(t, cmd->clip.rect);
            gfx_set_clip(r.x, r.y, r.w, r.h);
            break;
        }

        default: break;
    }
}

/* One canvas's commands.
 *
 * Walked directly rather than through mu_next_command(), which follows the
 * jump chain across every container - the entire point here is to paint one
 * container and leave the others alone. */
static void
paint_canvas(const mu_Container* cnt) {
    const char* p = (const char*)cnt->head + cnt->head->base.size;
    const char* end = (const char*)cnt->tail;

    while (p < end) {
        const mu_Command* cmd = (const mu_Command*)p;
        if (cmd->base.size <= 0) {
            break; /* corrupt list: stop rather than spin */
        }
        draw_command(cmd);
        p += cmd->base.size;
    }

    gfx_clear_clip();
}

/* FNV-1a. Cheap, and only ever run over the few hundred bytes a canvas's
 * commands occupy - the buffer is 8 KiB but almost none of it is used. */
static uint64_t
hash_canvas(const mu_Container* cnt) {
    const unsigned char* p = (const unsigned char*)cnt->head + cnt->head->base.size;
    const unsigned char* end = (const unsigned char*)cnt->tail;

    uint64_t h = 1469598103934665603ull;
    while (p < end) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

static bool
rects_overlap(mu_Rect a, mu_Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* cnt->rect is LOGICAL - ui_begin_screen() seeds it from
 * ui_width()/ui_height() - but every use below needs the PHYSICAL
 * footprint, same as draw_command() gets via ui_transform_rect(). Under
 * an odd quarter (GFX_WIDTH != GFX_HEIGHT) the rects differ in shape: an
 * unrotated (0,0,448,368) clipped onto a 368x448 framebuffer covers only
 * 368 of 448 rows, leaving 80 rows out of the background clear. Reported
 * on hardware as previous-frame pieces stuck after rotating Portrait to
 * Landscape. */
static mu_Rect
canvas_physical_rect(const mu_Container* cnt) {
    return ui_transform_rect(ui_effective_transform(), cnt->rect);
}

/* Which canvases changed. Also repaint one whose bands are already dirty:
 * something has drawn underneath it this frame, so its pixels are gone
 * however unchanged its own description is. */
static bool
canvas_itself_changed(const mu_Container* cnt) {
    const int slot = (int)(cnt - ui_ctx.containers);
    return ui_invalidated || slot < 0 || slot >= MU_CONTAINERPOOL_SIZE || hash_canvas(cnt) != ui_canvas_hash[slot];
}

static void
mark_changed_canvases(int n, bool* repaint) {
    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        const mu_Container* cnt = ui_ctx.root_list.items[i];
        const mu_Rect phys = canvas_physical_rect(cnt);

        if (canvas_itself_changed(cnt) || gfx_region_dirty(phys.x, phys.y, phys.w, phys.h)) {
            repaint[i] = true;
        }
    }
}

/* Painter's order: repainting a canvas erases whatever was drawn on top of
 * it, so everything above it that overlaps has to go again too. root_list is
 * sorted back to front by mu_end(). */
static void
propagate_repaint_over_overlaps(int n, bool* repaint) {
    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        if (!repaint[i]) {
            continue;
        }
        for (int j = i + 1; j < n && j < MU_ROOTLIST_SIZE; j++) {
            if (!repaint[j]
                && rects_overlap(canvas_physical_rect(ui_ctx.root_list.items[i]),
                                 canvas_physical_rect(ui_ctx.root_list.items[j]))) {
                repaint[j] = true;
            }
        }
    }
}

static bool
repaint_marked_canvases(int n, const bool* repaint, uint32_t background_rgb) {
    bool drew = false;

    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        if (!repaint[i]) {
            continue;
        }
        const mu_Container* cnt = ui_ctx.root_list.items[i];

        /* Clear only this canvas's rect, not the screen. Everything gfx draws
         * marks its own bands, so nothing else needs marking here. */
        if (background_rgb != UI_NO_BACKGROUND) {
            const mu_Rect phys = canvas_physical_rect(cnt);
            gfx_fill_rect(phys.x, phys.y, phys.w, phys.h, gfx_rgb(background_rgb));
        }

        paint_canvas(cnt);

        const int slot = (int)(cnt - ui_ctx.containers);
        if (slot >= 0 && slot < MU_CONTAINERPOOL_SIZE) {
            ui_canvas_hash[slot] = hash_canvas(cnt);
        }
        drew = true;
    }
    return drew;
}

bool
ui_end(uint32_t background_rgb) {
    mu_end(&ui_ctx);
    ui_pointer_state.over_scrollable = ui_ctx.scroll_target != NULL;

#if CONFIG_LAUNCHER_DEVELOPMENT
    report_command_list_high_water(ui_ctx.command_list.idx);
#endif

    const int n = ui_ctx.root_list.idx;
    bool repaint[MU_ROOTLIST_SIZE] = {false};

    mark_changed_canvases(n, repaint);
    propagate_repaint_over_overlaps(n, repaint);
    const bool drew = repaint_marked_canvases(n, repaint, background_rgb);

    ui_invalidated = false;
    return drew;
}

/* A backdrop is shared by every canvas, so once the UI itself has changed
 * it is painted once and every canvas goes over it. A UI that is unchanged
 * and only drawn under - the backdrop animating - is painted back on top
 * with no backdrop call: whoever drew under it has already drawn that part. */
bool
ui_end_over(ui_backdrop_fn paint_backdrop) {
    mu_end(&ui_ctx);
    ui_pointer_state.over_scrollable = ui_ctx.scroll_target != NULL;

#if CONFIG_LAUNCHER_DEVELOPMENT
    report_command_list_high_water(ui_ctx.command_list.idx);
#endif

    const int n = ui_ctx.root_list.idx;
    bool repaint[MU_ROOTLIST_SIZE] = {false};
    bool ui_changed = false;
    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        ui_changed = ui_changed || canvas_itself_changed(ui_ctx.root_list.items[i]);
    }

    if (ui_changed) {
        paint_backdrop();
        for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
            repaint[i] = true;
        }
    } else {
        mark_changed_canvases(n, repaint);
        propagate_repaint_over_overlaps(n, repaint);
    }
    const bool drew = repaint_marked_canvases(n, repaint, UI_NO_BACKGROUND);

    ui_invalidated = false;
    return drew;
}

/*
 * Band mode replay
 *
 * Band mode has no framebuffer to hash against, so ui_end()'s whole
 * changed/unchanged question does not apply - every band redraws every
 * frame regardless. What DOES matter is not walking or drawing a command
 * for a band it never reaches, the same reason a software rasterizer bins
 * shapes by row range instead of re-rasterizing the whole scene per band.
 */

typedef enum {
    UI_BAND_ENTRY_COMMAND,    /* replay via draw_command() */
    UI_BAND_ENTRY_FILL_RECT,  /* an opaque rect with no mu_Command behind it -
                                 a canvas's own background, or a queued
                                 overlay (ui_queue_band_overlay_rect()) */
    UI_BAND_ENTRY_CLIP_RESET, /* paint_canvas()'s own trailing gfx_clear_clip(),
                                  replayed at the same point in the stream */
} ui_band_entry_kind_t;

typedef struct {
    ui_band_entry_kind_t kind;
    const mu_Command* cmd; /* UI_BAND_ENTRY_COMMAND only */
    mu_Rect rect;          /* UI_BAND_ENTRY_FILL_RECT only */
    mu_Color color;        /* UI_BAND_ENTRY_FILL_RECT only */
    int y0, y1;            /* this entry's own row range - ignored when always is true */
    bool always;           /* replay regardless of a band's own range: CLIP_RESET,
                                and any command type command_row_range() does not
                                recognise (currently just MU_COMMAND_CLIP, whose
                                effect is state for what follows, not pixels of
                                its own) */
} ui_band_entry_t;

/* Headroom, not a tight fit - see gfx_dirty.h's own SUITE_MAX comment for
 * the same reasoning. A typical screen here is a handful of commands per
 * window plus one boundary marker; this leaves room for several such
 * windows in one frame. */
#define UI_BAND_BIN_MAX 64

static ui_band_entry_t ui_band_bin[UI_BAND_BIN_MAX];
static int ui_band_bin_count;

#define UI_EXTRA_RECT_MAX 4

typedef struct {
    int x, y, w, h;
    uint32_t rgb;
} ui_extra_rect_t;

static ui_extra_rect_t extra_rects[UI_EXTRA_RECT_MAX];
static int extra_rect_count;

void
ui_queue_band_overlay_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (extra_rect_count < UI_EXTRA_RECT_MAX) {
        extra_rects[extra_rect_count++] = (ui_extra_rect_t){x, y, w, h, rgb};
    }
}

/* The row range `cmd`'s own drawing would touch, after the transform -
 * geometry only, no colour or font-pass work, since binning only needs to
 * decide whether a band should bother calling draw_command() at all.
 * False means "no extent of its own" (MU_COMMAND_CLIP), which the caller
 * takes to mean "always replay". */
static bool
command_row_range(const mu_Command* cmd, int* y0, int* y1) {
    const ui_transform_t t = ui_effective_transform();

    switch (cmd->type) {
        case MU_COMMAND_RECT: {
            const mu_Rect r = ui_transform_rect(t, cmd->rect.rect);
            *y0 = r.y;
            *y1 = r.y + r.h;
            return true;
        }
        case MU_COMMAND_TEXT: {
            const ui_font_scaled_t fs = ui_resolve_font_scaled(cmd->text.font);
            const int tw = gfx_font_text_width(fs.font, cmd->text.str, -1, fs.scale);
            const int th = gfx_font_height(fs.font, fs.scale);
            const mu_Rect box = ui_transform_rect(t, (mu_Rect){cmd->text.pos.x, cmd->text.pos.y, tw, th});
            *y0 = box.y;
            *y1 = box.y + box.h;
            return true;
        }
        case MU_COMMAND_ICON: {
            const mu_Rect r = ui_transform_rect(t, cmd->icon.rect);
            *y0 = r.y;
            *y1 = r.y + r.h;
            return true;
        }
        default: return false;
    }
}

static void
bin_fill_rect(mu_Rect rect, mu_Color color) {
    if (ui_band_bin_count >= UI_BAND_BIN_MAX) {
        return;
    }
    ui_band_entry_t* e = &ui_band_bin[ui_band_bin_count++];
    e->kind = UI_BAND_ENTRY_FILL_RECT;
    e->rect = rect;
    e->color = color;
    e->y0 = rect.y;
    e->y1 = rect.y + rect.h;
    e->always = false;
}

/* One hash per possible band slot, sized for the smallest GFX_BAND_HEIGHT
 * (16) regardless of which one this build actually uses - a build using a
 * taller band simply leaves the tail unused. Compared and updated once per
 * ui_end_for_bands() call, this is the UI's own contribution to band
 * mode's per-band dirty decision (gfx_band_dirty(), gfx.c): a band whose
 * bound commands hash the same as last frame drew nothing new. */
#define UI_BAND_HASH_MAX (GFX_HEIGHT / 16)
static uint64_t ui_band_hash[UI_BAND_HASH_MAX];

/* FNV-1a, folded over whatever bytes make up an entry's own content -
 * the same algorithm hash_canvas() already uses for the same reason. */
static uint64_t
hash_bytes(uint64_t h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Hashes whichever bound entries overlap [row0, row1) - a COMMAND entry by
 * its own bytes (cmd->base.size is trustworthy: paint_canvas() already
 * relies on it the same way), a FILL_RECT by its rect and colour. Two
 * frames whose relevant entries hash the same drew the identical picture
 * in this band, mu_Command byte layout and all. */
static uint64_t
hash_band_entries(int row0, int row1) {
    uint64_t h = 1469598103934665603ull;

    for (int i = 0; i < ui_band_bin_count; i++) {
        const ui_band_entry_t* e = &ui_band_bin[i];
        if (!e->always && !(e->y0 < row1 && e->y1 > row0)) {
            continue;
        }
        h = hash_bytes(h, &e->kind, sizeof e->kind);
        switch (e->kind) {
            case UI_BAND_ENTRY_COMMAND: h = hash_bytes(h, e->cmd, (size_t)e->cmd->base.size); break;
            case UI_BAND_ENTRY_FILL_RECT:
                h = hash_bytes(h, &e->rect, sizeof e->rect);
                h = hash_bytes(h, &e->color, sizeof e->color);
                break;
            case UI_BAND_ENTRY_CLIP_RESET: break;
        }
    }
    return h;
}

/* The UI's own half of band mode's per-band dirty decision - marks a band
 * dirty (gfx_mark_dirty(), gfx.c) exactly when what would replay into it
 * changed since last frame, at that band's own full width: an entry's own
 * rect narrower than the band is not tracked per-entry here, only per-band. */
static void
mark_changed_ui_bands(void) {
    const int band_count = GFX_HEIGHT / GFX_BAND_HEIGHT;

    for (int b = 0; b < band_count && b < UI_BAND_HASH_MAX; b++) {
        const int row0 = b * GFX_BAND_HEIGHT;
        const uint64_t h = hash_band_entries(row0, row0 + GFX_BAND_HEIGHT);
        if (h != ui_band_hash[b]) {
            gfx_mark_dirty(0, row0, GFX_WIDTH, GFX_BAND_HEIGHT);
            ui_band_hash[b] = h;
        }
    }
}

void
ui_end_for_bands(uint32_t background_rgb) {
    mu_end(&ui_ctx);
    ui_pointer_state.over_scrollable = ui_ctx.scroll_target != NULL;

#if CONFIG_LAUNCHER_DEVELOPMENT
    report_command_list_high_water(ui_ctx.command_list.idx);
#endif

    ui_band_bin_count = 0;
    const int n = ui_ctx.root_list.idx;

    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        const mu_Container* cnt = ui_ctx.root_list.items[i];

        if (background_rgb != UI_NO_BACKGROUND) {
            bin_fill_rect(canvas_physical_rect(cnt), mu_color_from_rgb(background_rgb));
        }

        const char* p = (const char*)cnt->head + cnt->head->base.size;
        const char* end = (const char*)cnt->tail;
        while (p < end) {
            const mu_Command* cmd = (const mu_Command*)p;
            if (cmd->base.size <= 0) {
                break; /* corrupt list: stop rather than spin */
            }
            if (ui_band_bin_count < UI_BAND_BIN_MAX) {
                ui_band_entry_t* e = &ui_band_bin[ui_band_bin_count++];
                e->kind = UI_BAND_ENTRY_COMMAND;
                e->cmd = cmd;
                e->always = !command_row_range(cmd, &e->y0, &e->y1);
            }
            p += cmd->base.size;
        }

        if (ui_band_bin_count < UI_BAND_BIN_MAX) {
            ui_band_entry_t* e = &ui_band_bin[ui_band_bin_count++];
            e->kind = UI_BAND_ENTRY_CLIP_RESET;
            e->always = true;
        }
    }

    for (int i = 0; i < extra_rect_count; i++) {
        const ui_extra_rect_t* r = &extra_rects[i];
        bin_fill_rect(mu_rect(r->x, r->y, r->w, r->h), mu_color_from_rgb(r->rgb));
    }
    extra_rect_count = 0;

    mark_changed_ui_bands();
}

void
ui_replay_band(int row0, int row1) {
    for (int i = 0; i < ui_band_bin_count; i++) {
        const ui_band_entry_t* e = &ui_band_bin[i];
        if (!e->always && !(e->y0 < row1 && e->y1 > row0)) {
            continue;
        }
        switch (e->kind) {
            case UI_BAND_ENTRY_COMMAND: draw_command(e->cmd); break;
            case UI_BAND_ENTRY_FILL_RECT:
                gfx_fill_rect(e->rect.x, e->rect.y, e->rect.w, e->rect.h, mu_color_to_gfx(e->color));
                break;
            case UI_BAND_ENTRY_CLIP_RESET: gfx_clear_clip(); break;
        }
    }
}
