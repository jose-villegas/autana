/*
 * app_settings: the system settings list. It stores nothing: each row is its
 * owner's getter and setter, read every frame and called on a change, and an
 * owner that keeps its value persists it. Developer rows exist only in a
 * development build and reset at boot.
 */
#include "app/app.h"
#include "build/build_variant.h"
#include "display/display.h"
#include "gfx/present/gfx_present.h"
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#include "gfx/present/gfx_debug.h"
#endif
#include "shell/apps/system_settings/ui/settings_layout.h"
#include "shell/apps/system_settings/ui/settings_screen.h"
#include "ui/ui.h"

static bool
panel_clock_fast(void) {
    return display_system_panel_clock_hz() == GFX_PANEL_CLOCK_FAST_HZ;
}

static void
set_panel_clock_fast(bool fast) {
    display_set_system_panel_clock_hz(fast ? GFX_PANEL_CLOCK_FAST_HZ : GFX_PANEL_CLOCK_SLOW_HZ);
}

static const settings_toggle_t DISPLAY_ROWS[] = {
    {settings_panel_clock_label, panel_clock_fast, set_panel_clock_fast, settings_panel_clock_note,
     SETTINGS_NOTE_LINE_COUNT},
};

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
static const settings_toggle_t DEVELOPER_ROWS[] = {
    {settings_overlay_label, gfx_debug_overlay, gfx_set_debug_overlay, NULL, 0},
    {settings_leaf_label, gfx_debug_leaf_overlay, gfx_set_leaf_overlay, NULL, 0},
    {settings_interlace_label, gfx_interlace_enabled, gfx_set_interlace, NULL, 0},
    {settings_send_audit_label, gfx_send_audit, gfx_set_send_audit, NULL, 0},
};
#endif

static const settings_section_t SECTIONS[] = {
    {settings_display_heading, DISPLAY_ROWS, (int)(sizeof DISPLAY_ROWS / sizeof DISPLAY_ROWS[0])},
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    {settings_developer_heading, DEVELOPER_ROWS, (int)(sizeof DEVELOPER_ROWS / sizeof DEVELOPER_ROWS[0])},
#endif
};

static void
settings_enter(void) {}

static void
settings_frame(uint32_t dt_ms, const input_t* input) {
    (void)dt_ms;
    ui_begin(input);
    settings_screen_draw(SECTIONS, (int)(sizeof SECTIONS / sizeof SECTIONS[0]));
    ui_end(UI_BACKGROUND);
}

static void
settings_exit(void) {}

app_t app_settings = {
    .name = "Settings",
    .summary = "System settings",
    .enter = settings_enter,
    .frame = settings_frame,
    .exit = settings_exit,
    .invalidate = ui_invalidate,
    .home_gesture = true,
};

APP_REGISTER(app_settings);
