#include "ui/ui_icons.h"

#include <stddef.h>

#include "asset/asset_store.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char* TAG = "ui_icons";

static const char* const NAMES[UI_ICON_COUNT] = {
    [UI_ICON_CHECK] = "check",
    [UI_ICON_CLOSE] = "close",
    [UI_ICON_INFO] = "info",
    [UI_ICON_ALERT] = "alert",
    [UI_ICON_CHEVRON_LEFT] = "chevron_left",
    [UI_ICON_CHEVRON_RIGHT] = "chevron_right",
    [UI_ICON_CHEVRON_UP] = "chevron_up",
    [UI_ICON_CHEVRON_DOWN] = "chevron_down",
    [UI_ICON_ARROW_LEFT] = "arrow_left",
    [UI_ICON_CHECKBOX_ON] = "checkbox_on",
    [UI_ICON_HOME] = "home",
    [UI_ICON_WIFI] = "wifi",
    [UI_ICON_GAMEPAD] = "gamepad",
    [UI_ICON_MONITOR] = "monitor",
    [UI_ICON_PLUS] = "plus",
};

/* What an icon-only control shows in the icon's box without the pack. */
static const char* const TEXTS[UI_ICON_COUNT] = {
    [UI_ICON_CHECK] = "x",       [UI_ICON_CLOSE] = "X",        [UI_ICON_INFO] = "i",
    [UI_ICON_ALERT] = "!",       [UI_ICON_CHEVRON_LEFT] = "<", [UI_ICON_CHEVRON_RIGHT] = ">",
    [UI_ICON_CHEVRON_UP] = "^",  [UI_ICON_CHEVRON_DOWN] = "v", [UI_ICON_ARROW_LEFT] = "<",
    [UI_ICON_CHECKBOX_ON] = "x", [UI_ICON_HOME] = "H",         [UI_ICON_WIFI] = "W",
    [UI_ICON_GAMEPAD] = "G",     [UI_ICON_MONITOR] = "M",      [UI_ICON_PLUS] = "+",
};

bool
ui_icons_load(const char* pack, const char* const* names, int count, gfx_image_t* out) {
    const asset_pack_t* mounted = asset_store_pack(pack);
    if (mounted == NULL) {
        ESP_LOGW(TAG, "no pack %s: its icons are drawn as text", pack);
        return false;
    }
    for (int i = 0; i < count; i++) {
        asset_status_t status = gfx_image_from_pack(mounted, names[i], &out[i]);
        if (status == ASSET_OK && out[i].format != GFX_IMAGE_MONO1) {
            status = ASSET_ERR_FORMAT;
        }
        if (status != ASSET_OK) {
            ESP_LOGW(TAG, "icon %s/%s: %s: its icons are drawn as text", pack, names[i], asset_status_text(status));
            asset_store_release(pack);
            return false;
        }
    }
    return true;
}

const gfx_image_t*
ui_icon(ui_icon_id_t id) {
    static enum { UNTRIED, LOADED, MISSING } state = UNTRIED;

    static gfx_image_t icons[UI_ICON_COUNT];
    if (state == UNTRIED) {
        state = ui_icons_load(UI_ENGINE_PACK, NAMES, UI_ICON_COUNT, icons) ? LOADED : MISSING;
    }
    return state == LOADED ? &icons[id] : NULL;
}

const char*
ui_icon_text(ui_icon_id_t id) {
    return TEXTS[id];
}

void
ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color) {
    ui_draw_icon(ctx, r, ui_icon(id), ui_icon_text(id), color);
}
