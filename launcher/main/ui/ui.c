/*
 * ui: paints one microui frame into the real framebuffer. See ui.h for what
 * and why, and ui_build.c for the other half of this module: building the
 * frame this file paints, split out specifically because it needs none of
 * gfx and so can run on a host; see ui_build.c's own top comment.
 *
 * THE CANVAS MODEL
 *
 * A retained-mode engine knows what changed because changing it is an explicit
 * act: you mutate a node, the node marks itself dirty, and only its canvas is
 * rebuilt. Immediate mode throws that signal away by construction; the UI is
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
 * overlaps; otherwise the repaint erases what was on top.
 */

#include "ui/ui.h"
#include "profile/frame_cost.h"

#include <assert.h>
#include <limits.h>

#include "esp_log.h"

#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/target/gfx_target.h"
#include "math/scalar/mathi.h"
#include "services/tune.h"
#include "ui/controls/ui_icons.h"
#include "ui/input/ui_bridge.h"
#include "ui/ui_internal.h"

static uint32_t ui_band_fallbacks;

#if CONFIG_LAUNCHER_DEVELOPMENT
static const char* TAG = "ui";

static int command_list_high_water;

static void
report_command_list_high_water(int used) {
    if (used > command_list_high_water) {
        command_list_high_water = used;
        ESP_LOGI(TAG, "command list high water: %d / %d bytes", used, MU_COMMANDLIST_SIZE);
    }
}
#endif

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
TUNE_OWNER(ui);
TUNE(ui, band_stats_frames, 0, 0, 600);
static int64_t ui_band_cost_us;
static bool ui_band_stats_pending;
#endif

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
static void
report_band_stats(uint32_t fallbacks, int64_t cost_us) {
    ESP_LOGI(TAG, "band stats: fallbacks=%lu ui_us=%lld commands=%d roots=%d", (unsigned long)fallbacks,
             (long long)cost_us, ui_ctx.command_list.idx, ui_ctx.root_list.idx);
    band_stats_frames--;
}

static void
finish_band_stats(void) {
    if (ui_band_stats_pending && band_stats_frames > 0) {
        report_band_stats(ui_band_fallbacks, ui_band_cost_us);
    }
    ui_band_stats_pending = false;
}
#endif

/*
 * Painting
 *
 * Every command's geometry is mapped through the transform in force before
 * it reaches gfx; see ui_transform.h for what that buys, and ui_build.c's
 * ui_set_transform() for why an invalid one renders as identity rather than
 * being rejected at the point it was set.
 */

typedef struct {
    gfx_color_t color;
} icon_fill_ctx_t;

static void
icon_fill_emit(void* ctx_, recti_t rect) {
    const icon_fill_ctx_t* fc = ctx_;
    draw_rect(rect.x, rect.y, rect.w, rect.h, fc->color);
}

/* microui colour to the panel's own, alpha dropped; a style works in
 * microui's colour space and never sees a panel pixel; this is the one
 * place a command's colour crosses into gfx's. */
static gfx_color_t
mu_color_to_gfx(mu_Color c) {
    return gfx_rgb(ui_rgb_value(c));
}

static void
draw_text_command(const mu_Command* cmd, affine2x_t t) {
    const mu_Color ink = cmd->text.color;
    const mu_Color halo = ui_text_halo(ink);
    const ui_font_scaled_t fs = ui_resolve_font_scaled(cmd->text.font);
    const draw_font_t* font = fs.font;
    const int scale = fs.scale;
    const int quarter = ui_transform_quarter(t);

    /* WHY THE WHOLE STRING'S BOX IS MAPPED, NOT ITS ORIGIN: every
     * other command maps its rect through ui_transform_rect(), proven
     * exact under any quarter turn. A point does not commute with
     * "walk N glyphs, take the far edge" under rotation, so mapping
     * just the origin and walking per-glyph from there drifts a
     * string off at quarter turns 1-3. Mapping the LOGICAL box
     * instead (the same one used to size it) keeps it exact, like
     * every other command. */
    const int tw = draw_font_text_width(font, cmd->text.str, -1, scale);
    const int th = draw_font_height(font, scale);
    const mu_Rect box = ui_transform_mu_rect(t, (mu_Rect){cmd->text.pos.x, cmd->text.pos.y, tw, th});

    /* draw_text_font()'s (x, y) is the FIRST GLYPH's cell, not a
     * corner of the box; see ui_text_glyph0_origin()'s own comment
     * (ui_transform.h) for the full derivation. Extracted there, not
     * kept inline, so it's testable against a synthetic proportional
     * font on a host: a port of the same origin math an app's own
     * label-drawing code solves for itself, ported rather than called
     * directly because ui/ sits below apps/, so reaching into an
     * app's source would be a backwards layering dependency. */
    int mx;
    int my;
    ui_text_glyph0_origin(font, ui_recti_from_mu(box), quarter, scale, &mx, &my);

    /* A text colour's alpha is dithered coverage. No halo has a
     * dithered form, so a fading string is drawn as ink alone. */
    if (ink.a < 255) {
        if (ink.a > 0) {
            draw_text_font_dither(mx, my, cmd->text.str, mu_color_to_gfx(ink), scale, quarter, font, ink.a);
        }
        return;
    }

    if (ui_text_style == UI_TEXT_OUTLINED) {
        /* Ink follows the halo so the outline does not cover glyphs. */
        const gfx_color_t halo_color = mu_color_to_gfx(halo);
        const gfx_color_t ink_color = mu_color_to_gfx(ink);
        draw_text_font_halo(mx, my, cmd->text.str, halo_color, scale, quarter, font);
        draw_text_font(mx, my, cmd->text.str, ink_color, scale, quarter, font);
        return;
    }

    ui_text_pass_t passes[UI_TEXT_MAX_PASSES];
    const int n = ui_text_passes(ui_text_style, passes, UI_TEXT_MAX_PASSES);
    for (int i = 0; i < n; i++) {
        const mu_Color c = passes[i].ink ? ink : halo;
        const gfx_color_t color = mu_color_to_gfx(c);

        /* THE HALO OFFSET IS ADDED AFTER THE MAPPING, NOT BEFORE.
         * passes[i].dx/dy is a SCREEN-SPACE offset; see ui_style.h's
         * OUTLINED/SHADOWED passes, which sit a halo a fixed pixel
         * count from the glyph on the panel. (mx, my) already IS a
         * screen position. Transforming (dx, dy) itself would instead
         * rotate the halo with the glyph: a shadow meant to fall
         * down-and-right on screen would fall down-and-right in
         * LOGICAL space instead, a different physical direction once
         * turn is nonzero. */
        draw_text_font(mx + passes[i].dx, my + passes[i].dy, cmd->text.str, color, scale, quarter, font);
    }
}

static void
draw_command(const mu_Command* cmd) {
    const affine2x_t t = ui_effective_transform();

    switch (cmd->type) {

        case MU_COMMAND_RECT: {
            const mu_Color c = cmd->rect.color;
            /* Fully transparent rects are microui's way of drawing nothing; we
         * have no blending, so skip them rather than paint black. */
            if (c.a == 0) {
                break;
            }
            const mu_Rect r = ui_transform_mu_rect(t, cmd->rect.rect);
            draw_rect(r.x, r.y, r.w, r.h, mu_color_to_gfx(c));
            break;
        }

        case MU_COMMAND_TEXT: draw_text_command(cmd, t); break;

        case MU_COMMAND_ICON: {
            /* microui's icons are close/check/collapsed/expanded. MU_ICON_CHECK
             * is the system check icon because two callers need it: a
             * checkbox toggle, and a per-tile selection badge. The other
             * three, and the check without the engine pack, are a small
             * centred square: nothing in this shell closes a window or
             * collapses a tree yet to ask for them. */
            const mu_Color c = cmd->icon.color;
            const gfx_color_t color = mu_color_to_gfx(c);
            const draw_image_t* check = cmd->icon.id == MU_ICON_CHECK ? ui_icon_image(UI_ICON_CHECK) : NULL;
            if (check != NULL) {
                icon_fill_ctx_t fc = {.color = color};
                ui_transform_icon_blocks(t, check, ui_recti_from_mu(cmd->icon.rect), icon_fill_emit, &fc);
            } else {
                const mu_Rect r = ui_transform_mu_rect(t, cmd->icon.rect);
                draw_rect(r.x + (r.w / 3), r.y + (r.h / 3), r.w / 3, r.h / 3, color);
            }
            break;
        }

        case MU_COMMAND_CLIP: {
            const mu_Rect r = ui_transform_mu_rect(t, cmd->clip.rect);
            draw_set_clip(r.x, r.y, r.w, r.h);
            break;
        }

        default: break;
    }
}

/* Nested heads skip their canvas. Stop at this root's tail before its
 * painter-order jump: mu_next_command() continues into other roots. */
static bool
next_canvas_command(const mu_Container* cnt, mu_Command** cmd) {
    if (*cmd == NULL) {
        *cmd = cnt->head;
    }
    assert((*cmd)->base.size > 0);
    *cmd = (mu_Command*)((char*)*cmd + (*cmd)->base.size);
    while ((const char*)*cmd < (const char*)cnt->tail) {
        if ((*cmd)->type != MU_COMMAND_JUMP) {
            return true;
        }
        assert((const char*)(*cmd)->jump.dst > (const char*)*cmd);
        *cmd = (*cmd)->jump.dst;
    }
    return false;
}

#ifndef ESP_PLATFORM
void (*ui_canvas_painted_for_test)(const mu_Container*);
#endif

static void
paint_canvas(const mu_Container* cnt) {
    mu_Command* cmd = NULL;
    while (next_canvas_command(cnt, &cmd)) {
        draw_command(cmd);
    }
    draw_clear_clip();
#ifndef ESP_PLATFORM
    if (ui_canvas_painted_for_test != NULL) {
        ui_canvas_painted_for_test(cnt);
    }
#endif
}

/* FNV-1a (Fowler, Noll, Vo). Cheap, and only ever run over the few
 * hundred bytes a canvas's commands occupy; the buffer is 8 KiB but almost
 * none of it is used. */
static uint64_t
hash_canvas(const mu_Container* cnt) {
    uint64_t h = 1469598103934665603ULL;
    mu_Command* cmd = NULL;
    while (next_canvas_command(cnt, &cmd)) {
        const unsigned char* p = (const unsigned char*)cmd;
        for (int i = 0; i < cmd->base.size; i++) {
            h = (h ^ p[i]) * 1099511628211ULL;
        }
    }
    return h;
}

static bool
rects_overlap(mu_Rect a, mu_Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* cnt->rect is LOGICAL (ui_begin_screen() seeds it from
 * ui_width()/ui_height()) but every use below needs the PHYSICAL
 * footprint, same as draw_command() gets via ui_transform_rect(). Under
 * an odd quarter (GFX_WIDTH != GFX_HEIGHT) the rects differ in shape: an
 * unrotated (0,0,448,368) clipped onto a 368x448 framebuffer covers only
 * 368 of 448 rows, leaving 80 rows out of the background clear. Reported
 * on hardware as previous-frame pieces stuck after rotating Portrait to
 * Landscape. */
static mu_Rect
canvas_physical_rect(const mu_Container* cnt) {
    return ui_transform_mu_rect(ui_effective_transform(), cnt->rect);
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

/* The rects of every window painted before but not drawn this frame, each
 * reported once. Nothing else repaints them: see ui_canvas_marks.h. */
static int
take_vanished_canvases(mu_Rect* out) {
    bool present[MU_CONTAINERPOOL_SIZE] = {false};
    for (int i = 0; i < ui_ctx.root_list.idx && i < MU_ROOTLIST_SIZE; i++) {
        const int slot = (int)(ui_ctx.root_list.items[i] - ui_ctx.containers);
        if (slot >= 0 && slot < MU_CONTAINERPOOL_SIZE) {
            present[slot] = true;
        }
    }
    return ui_canvas_marks_take_vanished(&ui_canvas_marks, present, out, MU_CONTAINERPOOL_SIZE);
}

/* Clearing a vanished rect marks it dirty, so whatever overlaps it repaints
 * in mark_changed_canvases(). With no background to clear to, the app
 * underneath is asked for a full redraw instead. */
static void
clear_vanished_canvases(uint32_t background_rgb) {
    mu_Rect vanished[MU_CONTAINERPOOL_SIZE];
    const int n = take_vanished_canvases(vanished);
    for (int i = 0; i < n; i++) {
        if (background_rgb != UI_NO_BACKGROUND) {
            draw_rect(vanished[i].x, vanished[i].y, vanished[i].w, vanished[i].h, gfx_rgb(background_rgb));
        } else {
            gfx_request_full_redraw();
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
            draw_rect(phys.x, phys.y, phys.w, phys.h, gfx_rgb(background_rgb));
        }

        paint_canvas(cnt);

        const int slot = (int)(cnt - ui_ctx.containers);
        if (slot >= 0 && slot < MU_CONTAINERPOOL_SIZE) {
            ui_canvas_hash[slot] = hash_canvas(cnt);
        }
        ui_canvas_marks_painted(&ui_canvas_marks, slot, canvas_physical_rect(cnt));
        drew = true;
    }
    return drew;
}

void
ui_end_commands(void) {
    UI_FACE_COST_BEGIN(backend_from);
    ui_face_end();
    mu_end(&ui_ctx);
    ui_end_pointer_frame();
    UI_FACE_COST_END(backend_from, "ui.backend");

#if CONFIG_LAUNCHER_DEVELOPMENT
    report_command_list_high_water(ui_ctx.command_list.idx);
#endif
}

bool
ui_end(uint32_t background_rgb) {
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    finish_band_stats();
#endif
    ui_band_fallbacks = 0;
    /* No framebuffer to paint: the bands replay it as gfx sends them. */
    if (gfx_frame_expanded() || gfx_mode_current()->layout == GFX_LAYOUT_BANDS) {
        ui_end_for_bands(background_rgb);
        return true;
    }
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t began_us = band_stats_frames > 0 ? timing_now_us() : 0;
#endif
    ui_end_commands();
    FRAME_COST_BEGIN(paint_from);

    const int n = ui_ctx.root_list.idx;
    bool repaint[MU_ROOTLIST_SIZE] = {false};

    clear_vanished_canvases(background_rgb);
    mark_changed_canvases(n, repaint);
    propagate_repaint_over_overlaps(n, repaint);
    const bool drew = repaint_marked_canvases(n, repaint, background_rgb);

    ui_invalidated = false;
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    if (began_us != 0) {
        report_band_stats(0, timing_now_us() - began_us);
    }
#endif
    FRAME_COST_END(paint_from, "ui.paint");
    return drew;
}

/* A backdrop is shared by every canvas, so once the UI itself has changed
 * it is painted once and every canvas goes over it. A UI that is unchanged
 * and only drawn under (the backdrop animating) is painted back on top
 * with no backdrop call: whoever drew under it has already drawn that part. */
bool
ui_end_over(ui_backdrop_fn paint_backdrop) {
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    finish_band_stats();
#endif
    ui_band_fallbacks = 0;
    assert(!gfx_frame_expanded());
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t began_us = band_stats_frames > 0 ? timing_now_us() : 0;
#endif
    ui_end_commands();
    FRAME_COST_BEGIN(paint_from);

    const int n = ui_ctx.root_list.idx;
    bool repaint[MU_ROOTLIST_SIZE] = {false};
    mu_Rect vanished[MU_CONTAINERPOOL_SIZE];
    bool ui_changed = take_vanished_canvases(vanished) > 0;
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
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    if (began_us != 0) {
        report_band_stats(0, timing_now_us() - began_us);
    }
#endif
    FRAME_COST_END(paint_from, "ui.paint");
    return drew;
}

/*
 * Band mode replay
 *
 * Band mode has no framebuffer to hash against, so ui_end()'s whole
 * changed/unchanged question does not apply; every band redraws every
 * frame regardless. What DOES matter is not walking or drawing a command
 * for a band it never reaches, the same reason a software rasterizer bins
 * shapes by row range instead of re-rasterizing the whole scene per band.
 */

typedef enum {
    UI_BAND_ENTRY_COMMAND,    /* replay via draw_command() */
    UI_BAND_ENTRY_FILL_RECT,  /* an opaque rect with no mu_Command behind it:
                                 a canvas's own background, or a queued
                                 overlay (ui_queue_band_overlay_rect()) */
    UI_BAND_ENTRY_CLIP_RESET, /* paint_canvas()'s own trailing draw_clear_clip(),
                                  replayed at the same point in the stream */
} ui_band_entry_kind_t;

typedef struct {
    union {
        const mu_Command* cmd;

        struct {
            mu_Rect rect;
            mu_Color color;
        };
    };

    ui_band_entry_kind_t kind;
    int y0, y1;  /* this entry's own row range, ignored when always is true */
    bool always; /* replay regardless of a band's own range: UI_BAND_ENTRY_CLIP_RESET,
                                and any command type command_row_range() does not
                                recognise (currently just MU_COMMAND_CLIP, whose
                                effect is state for what follows, not pixels of
                                its own) */
} ui_band_entry_t;

#define UI_EXTRA_RECT_MAX 4

static ui_band_entry_t* ui_band_bin;
static bool ui_band_bin_initialized;

void
ui_band_init(void) {
    if (!ui_band_bin_initialized) {
        ui_band_bin_initialized = true;
        ui_band_bin = memory_alloc(UI_BAND_BIN_MAX * sizeof *ui_band_bin, MEMORY_PSRAM);
    }
}

static int ui_band_bin_count;
static uint32_t ui_band_overflow;
static uint32_t ui_band_background;
static bool ui_band_active;

typedef struct {
    recti_t rect;
    uint32_t rgb;
} ui_extra_rect_t;

static ui_extra_rect_t extra_rects[UI_EXTRA_RECT_MAX];
static int extra_rect_count;
static int replay_rect_count;

void
ui_queue_band_overlay_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (extra_rect_count < UI_EXTRA_RECT_MAX) {
        extra_rects[extra_rect_count++] = (ui_extra_rect_t){{x, y, w, h}, rgb};
    }
}

/* The row range `cmd`'s own drawing would touch, after the transform:
 * geometry only, no colour or font-pass work, since binning only needs to
 * decide whether a band should bother calling draw_command() at all.
 * False means "no extent of its own" (MU_COMMAND_CLIP), which the caller
 * takes to mean "always replay". */
static bool
command_row_range(const mu_Command* cmd, int* y0, int* y1) {
    const affine2x_t t = ui_effective_transform();

    switch (cmd->type) {
        case MU_COMMAND_RECT: {
            const mu_Rect r = ui_transform_mu_rect(t, cmd->rect.rect);
            *y0 = r.y;
            *y1 = r.y + r.h;
            return true;
        }
        case MU_COMMAND_TEXT: {
            const ui_font_scaled_t fs = ui_resolve_font_scaled(cmd->text.font);
            const int tw = draw_font_text_width(fs.font, cmd->text.str, -1, fs.scale);
            const int th = draw_font_height(fs.font, fs.scale);
            const mu_Rect box = ui_transform_mu_rect(t, (mu_Rect){cmd->text.pos.x, cmd->text.pos.y, tw, th});
            *y0 = box.y;
            *y1 = box.y + box.h;
            if (cmd->text.color.a == 255) {
                ui_text_pass_t passes[UI_TEXT_MAX_PASSES];
                const int n = ui_text_passes(ui_text_style, passes, UI_TEXT_MAX_PASSES);
                for (int i = 0; i < n; i++) {
                    *y0 = mathi_min(*y0, box.y + passes[i].dy);
                    *y1 = mathi_max(*y1, box.y + box.h + passes[i].dy);
                }
            }
            return true;
        }
        case MU_COMMAND_ICON: {
            const mu_Rect r = ui_transform_mu_rect(t, cmd->icon.rect);
            *y0 = r.y;
            *y1 = r.y + r.h;
            return true;
        }
        default: return false;
    }
}

/* Omitted clip commands carry state, but only omitted drawing can change
 * pixels beyond the retained prefix. Those bands replay the complete stream. */
static ui_band_entry_t*
bin_entry(const ui_band_entry_t* entry) {
    if (ui_band_bin != NULL && ui_band_bin_count < UI_BAND_BIN_MAX) {
        ui_band_entry_t* stored = &ui_band_bin[ui_band_bin_count++];
        *stored = *entry;
        return stored;
    }
    if (!entry->always) {
        for (int b = 0; b < GFX_HEIGHT / GFX_BAND_HEIGHT; b++) {
            const int row0 = b * GFX_BAND_HEIGHT;
            if (entry->y0 < row0 + GFX_BAND_HEIGHT && entry->y1 > row0) {
                ui_band_overflow |= UINT32_C(1) << b;
            }
        }
    }
    return NULL;
}

static void
bin_fill_rect(mu_Rect rect, mu_Color color) {
    const ui_band_entry_t entry = {
        .kind = UI_BAND_ENTRY_FILL_RECT, .rect = rect, .color = color, .y0 = rect.y, .y1 = rect.y + rect.h};
    (void)bin_entry(&entry);
}

/* Overflowed bands cannot hash omitted content, so remain dirty until their
 * complete stream fits the bin again. Other bands retain their hashes. */
#define UI_BAND_HASH_MAX (GFX_HEIGHT / GFX_BAND_HEIGHT)
_Static_assert(UI_BAND_HASH_MAX <= sizeof(uint32_t) * CHAR_BIT, "overflow mask holds every band");
static uint64_t ui_band_hash[UI_BAND_HASH_MAX];

/* FNV-1a, folded over whatever bytes make up an entry's own content,
 * the same algorithm hash_canvas() already uses for the same reason. */
static uint64_t
hash_bytes(uint64_t h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Hashes whichever bound entries overlap [row0, row1): a COMMAND entry by
 * its own bytes (cmd->base.size is trustworthy: paint_canvas() already
 * relies on it the same way), a UI_BAND_ENTRY_FILL_RECT by its rect and
 * colour. Two
 * frames whose relevant entries hash the same drew the identical picture
 * in this band, mu_Command byte layout and all. */
static uint64_t
hash_band_entries(int row0, int row1) {
    uint64_t h = 1469598103934665603ULL;

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

/* The UI's own half of band mode's per-band dirty decision: marks a band
 * dirty (gfx_mark_dirty(), gfx_present.c) exactly when what would replay into it
 * changed since last frame, at that band's own full width: an entry's own
 * rect narrower than the band is not tracked per-entry here, only per-band. */
static void
mark_changed_ui_bands(void) {
    const gfx_mode_t* const mode = gfx_mode_current();
    if (mode->layout != GFX_LAYOUT_BANDS) {
        return;
    }
    const int band_count = mode->height / mode->band_height;

    for (int b = 0; b < band_count && b < UI_BAND_HASH_MAX; b++) {
        const int row0 = b * mode->band_height;
        const uint64_t h = hash_band_entries(row0, row0 + mode->band_height);
        const bool overflow = (ui_band_overflow & (UINT32_C(1) << b)) != 0;
        if (overflow || h != ui_band_hash[b]) {
            gfx_mark_dirty(0, row0, mode->width, mode->band_height);
            ui_band_hash[b] = overflow ? 0 : h;
        }
    }
}

void
ui_clear_band_overlay(void) {
    ui_band_bin_count = 0;
    extra_rect_count = 0;
    ui_band_active = false;
    ui_band_overflow = 0;
}

void
ui_end_for_bands(uint32_t background_rgb) {
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    finish_band_stats();
    const int64_t began_us = band_stats_frames > 0 ? timing_now_us() : 0;
#endif
    ui_band_active = true;
    ui_band_overflow = 0;
    ui_band_fallbacks = 0;
    ui_band_background = background_rgb;
    ui_end_commands();
    FRAME_COST_BEGIN(paint_from);

    ui_band_bin_count = 0;
    const int n = ui_ctx.root_list.idx;

    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        const mu_Container* cnt = ui_ctx.root_list.items[i];

        if (background_rgb != UI_NO_BACKGROUND) {
            bin_fill_rect(canvas_physical_rect(cnt), ui_rgb(background_rgb));
        }

        mu_Command* cmd = NULL;
        while (next_canvas_command(cnt, &cmd)) {
            ui_band_entry_t entry = {.kind = UI_BAND_ENTRY_COMMAND, .cmd = cmd};
            entry.always = !command_row_range(cmd, &entry.y0, &entry.y1);
            (void)bin_entry(&entry);
        }

        const ui_band_entry_t reset = {.kind = UI_BAND_ENTRY_CLIP_RESET, .cmd = cnt->tail, .always = true};
        (void)bin_entry(&reset);
    }

    for (int i = 0; i < extra_rect_count; i++) {
        const ui_extra_rect_t* r = &extra_rects[i];
        bin_fill_rect(ui_mu_from_recti(r->rect), ui_rgb(r->rgb));
    }
    replay_rect_count = extra_rect_count;
    extra_rect_count = 0;
    mark_changed_ui_bands();
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    ui_band_cost_us = began_us != 0 ? timing_now_us() - began_us : 0;
    ui_band_stats_pending = began_us != 0;
#endif
    FRAME_COST_END(paint_from, "ui.paint");
}

static bool
band_overflows(int row0, int row1) {
    for (int b = 0; b < UI_BAND_HASH_MAX; b++) {
        if (b * GFX_BAND_HEIGHT < row1 && (b + 1) * GFX_BAND_HEIGHT > row0
            && (ui_band_overflow & (UINT32_C(1) << b)) != 0) {
            return true;
        }
    }
    return false;
}

static void
replay_full_band(void) {
    for (int i = 0; i < ui_ctx.root_list.idx; i++) {
        const mu_Container* cnt = ui_ctx.root_list.items[i];
        if (ui_band_background != UI_NO_BACKGROUND) {
            const mu_Rect r = canvas_physical_rect(cnt);
            draw_rect(r.x, r.y, r.w, r.h, gfx_rgb(ui_band_background));
        }
        paint_canvas(cnt);
    }
    for (int i = 0; i < replay_rect_count; i++) {
        const ui_extra_rect_t* r = &extra_rects[i];
        draw_rect(r->rect.x, r->rect.y, r->rect.w, r->rect.h, gfx_rgb(r->rgb));
    }
}

static void
replay_binned_band(int row0, int row1) {
    for (int i = 0; i < ui_band_bin_count; i++) {
        const ui_band_entry_t* e = &ui_band_bin[i];
        if (!e->always && !(e->y0 < row1 && e->y1 > row0)) {
            continue;
        }
        switch (e->kind) {
            case UI_BAND_ENTRY_COMMAND: draw_command(e->cmd); break;
            case UI_BAND_ENTRY_FILL_RECT:
                draw_rect(e->rect.x, e->rect.y, e->rect.w, e->rect.h, mu_color_to_gfx(e->color));
                break;
            case UI_BAND_ENTRY_CLIP_RESET: draw_clear_clip();
#ifndef ESP_PLATFORM
                if (ui_canvas_painted_for_test != NULL) {
                    for (int root = 0; root < ui_ctx.root_list.idx; root++) {
                        const mu_Container* cnt = ui_ctx.root_list.items[root];
                        if (cnt->tail == e->cmd) {
                            ui_canvas_painted_for_test(cnt);
                        }
                    }
                }
#endif
                break;
        }
    }
    draw_clear_clip();
}

void
ui_replay_band(int row0, int row1) {
    if (!ui_band_active) {
        return;
    }
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t began_us = ui_band_stats_pending && band_stats_frames > 0 ? timing_now_us() : 0;
#endif
    if (band_overflows(row0, row1)) {
        ui_band_fallbacks++;
        replay_full_band();
    } else {
        replay_binned_band(row0, row1);
    }

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    if (began_us != 0) {
        ui_band_cost_us += timing_now_us() - began_us;
    }
#endif
}

uint32_t
ui_band_fallback_count(void) {
    return ui_band_fallbacks;
}
