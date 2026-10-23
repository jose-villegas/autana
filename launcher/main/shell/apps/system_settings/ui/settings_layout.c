#include "shell/apps/system_settings/ui/settings_layout.h"

const char settings_display_heading[] = "DISPLAY";
const char settings_developer_heading[] = "DEVELOPER";
const char settings_panel_clock_label[] = "fast panel clock";
const char settings_overlay_label[] = "grid overlay";
const char settings_leaf_label[] = "leaf overlay";
const char settings_interlace_label[] = "interlace";
const char settings_send_audit_label[] = "send audit";
const char* const settings_panel_clock_note[SETTINGS_NOTE_LINE_COUNT] = {
    "80 MHz is faster;", "apps that redraw", "only what changed", "may show stray", "pixels or lines.",
};
