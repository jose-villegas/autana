/* shell_frame: the shell chrome drawn into a framebuffer or replayed over expanded strips. */
#include <stdio.h>

#include "display/display.h"
#include "gfx/gfx.h"
#include "shell/shell_frame.h"
#include "ui/ui.h"
#include "ui/ui_anchor.h"
#include "util/runtime/build_id.h"

#if CONFIG_LAUNCHER_DEVELOPMENT || !defined(ESP_PLATFORM)
#define BUILD_MARK_GLYPH        8
#define BUILD_MARK_CHARS        (1 + BUILD_ID_SHORT_CHARS)
#define BUILD_MARK_SIZE         (BUILD_MARK_GLYPH * BUILD_MARK_CHARS)
#define BUILD_MARK_RGB          0x384054
/* The panel's rounded corners hide more than UI_MARGIN clears along an edge. */
#define BUILD_MARK_CORNER_SHIFT 32

static char build_mark_text[BUILD_MARK_CHARS + 1];

static void
draw_build_mark(void) {
    const int quarter = display_quarter_now();
    const int screen_w = (quarter % 2 == 0) ? GFX_WIDTH : GFX_HEIGHT;
    const int screen_h = (quarter % 2 == 0) ? GFX_HEIGHT : GFX_WIDTH;
    const mu_Rect upright =
        ui_anchor_rect((mu_Rect){0, 0, screen_w, screen_h}, UI_ANCHOR_BOTTOM_RIGHT, UI_ANCHOR_BOTTOM_RIGHT,
                       -UI_MARGIN - BUILD_MARK_CORNER_SHIFT, -UI_MARGIN, BUILD_MARK_SIZE, BUILD_MARK_GLYPH);
    const mu_Rect box = ui_transform_rect(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT), upright);

    if (gfx_region_dirty(box.x, box.y, box.w, box.h)) {
        int x = 0;
        int y = 0;
        ui_text_glyph0_origin(gfx_font_ui(), box, quarter, 1, &x, &y);
        gfx_text_turned(x, y, build_mark_text, gfx_rgb(BUILD_MARK_RGB), 1, quarter);
    }
}
#endif

void
shell_frame_init(const char* short_id) {
#if CONFIG_LAUNCHER_DEVELOPMENT || !defined(ESP_PLATFORM)
    const int length = snprintf(build_mark_text, sizeof(build_mark_text), "D%s", short_id);
    if (length < 0 || (size_t)length >= sizeof(build_mark_text)) {
        build_mark_text[0] = '\0';
    }
#else
    (void)short_id;
#endif
}

void
shell_frame_overlay(int row0, int row1) {
    ui_replay_band(row0, row1);
#if CONFIG_LAUNCHER_DEVELOPMENT || !defined(ESP_PLATFORM)
    draw_build_mark();
#endif
}

void
shell_frame_extras(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT || !defined(ESP_PLATFORM)
    if (gfx_mode_current()->layout == GFX_LAYOUT_FULL_FB && !gfx_frame_expanded()) {
        draw_build_mark();
    }
#endif
}
