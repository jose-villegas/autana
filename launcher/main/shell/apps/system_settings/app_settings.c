/* A view of display preferences and development graphics instruments. */
#include "app/app.h"
#include "build/build_variant.h"
#include "display/display.h"
#include "gfx/present/gfx_present.h"
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#include "gfx/present/gfx_debug.h"
#endif
#include "shell/apps/system_settings/settings_ui.h"
#include "shell/apps/system_settings/ui/settings_layout.h"
#include "shell/apps/system_settings/ui/settings_screen.h"
#include "ui/ui.h"

static void
settings_enter(void) {}

static void
settings_frame(uint32_t dt_ms, const input_t* input) {
    (void)dt_ms;
    const settings_snapshot_t snapshot = {
        .fast_clock = display_system_panel_clock_hz() == GFX_PANEL_CLOCK_FAST_HZ,
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
        .overlay_on = gfx_debug_overlay(),
        .leaf_on = gfx_debug_leaf_overlay(),
        .interlace_on = gfx_interlace_enabled(),
        .send_audit_on = gfx_send_audit(),
#endif
    };
    ui_begin(input);
    const settings_result_t result = settings_screen_draw(&snapshot);
    ui_end(SETTINGS_BACKGROUND);
    const settings_changes_t changes = settings_ui_changes(&snapshot, &result);
    if (changes.panel_clock) {
        display_set_system_panel_clock_hz(result.values.fast_clock ? GFX_PANEL_CLOCK_FAST_HZ : GFX_PANEL_CLOCK_SLOW_HZ);
    }
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (changes.overlay) {
        gfx_set_debug_overlay(result.values.overlay_on);
    }
    if (changes.leaf) {
        gfx_set_leaf_overlay(result.values.leaf_on);
    }
    if (changes.interlace) {
        gfx_set_interlace(result.values.interlace_on);
    }
    if (changes.send_audit) {
        gfx_set_send_audit(result.values.send_audit_on);
    }
#endif
}

static void
settings_exit(void) {}

app_t settings_app = {
    .name = "Settings",
    .summary = "System settings",
    .enter = settings_enter,
    .frame = settings_frame,
    .exit = settings_exit,
    .invalidate = ui_invalidate,
    .home_gesture = true,
};

APP_REGISTER(settings_app);
