#include "ui/ui_icons.h"

#include <stddef.h>

#include "asset/asset_store.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char* TAG = "ui_icons";

static const ui_icon_name_t SYSTEM_NAMES[UI_ICON_COUNT] = {
    [UI_ICON_CHECK] = {"check", "x"},
    [UI_ICON_CLOSE] = {"close", "X"},
    [UI_ICON_INFO] = {"info", "i"},
    [UI_ICON_ALERT] = {"alert", "!"},
    [UI_ICON_CHEVRON_LEFT] = {"chevron_left", "<"},
    [UI_ICON_CHEVRON_RIGHT] = {"chevron_right", ">"},
    [UI_ICON_CHEVRON_UP] = {"chevron_up", "^"},
    [UI_ICON_CHEVRON_DOWN] = {"chevron_down", "v"},
    [UI_ICON_ARROW_LEFT] = {"arrow_left", "<"},
    [UI_ICON_CHECKBOX_ON] = {"checkbox_on", "x"},
    [UI_ICON_HOME] = {"home", "H"},
    [UI_ICON_WIFI] = {"wifi", "W"},
    [UI_ICON_GAMEPAD] = {"gamepad", "G"},
    [UI_ICON_MONITOR] = {"monitor", "M"},
    [UI_ICON_PLUS] = {"plus", "+"},
};

static gfx_image_t system_icons[UI_ICON_COUNT];
static ui_icon_set_t system_set = {UI_ENGINE_PACK, SYSTEM_NAMES, UI_ICON_COUNT, system_icons, false};

bool
ui_icon_set_load(ui_icon_set_t* set) {
    if (set->loaded) {
        return true;
    }
    /* A missing pack is logged by the store. */
    const asset_pack_t* pack = asset_store_pack(set->pack);
    if (pack == NULL) {
        return false;
    }
    for (int i = 0; i < set->count; i++) {
        const char* name = set->names[i].name;
        asset_view_t entry;
        asset_status_t status = asset_pack_find(pack, name, GFX_IMAGE_ASSET, &entry);
        if (status == ASSET_OK) {
            status = gfx_image_open(entry, &set->icons[i]);
        }
        if (status == ASSET_OK && set->icons[i].format != GFX_IMAGE_MONO1) {
            status = ASSET_ERR_FORMAT;
        }
        if (status != ASSET_OK) {
            ESP_LOGW(TAG, "icon %s/%s: %s: the set is drawn as text", set->pack, name, asset_status_text(status));
            asset_store_release(set->pack);
            return false;
        }
    }
    set->loaded = true;
    return true;
}

void
ui_icon_set_release(ui_icon_set_t* set) {
    if (set->loaded) {
        asset_store_release(set->pack);
        set->loaded = false;
    }
}

const gfx_image_t*
ui_icon_set_get(const ui_icon_set_t* set, int i) {
    return set->loaded ? &set->icons[i] : NULL;
}

void
ui_icon_set_draw(mu_Context* ctx, mu_Rect r, const ui_icon_set_t* set, int i, mu_Color color) {
    ui_draw_icon(ctx, r, ui_icon_set_get(set, i), set->names[i].text, color);
}

void
ui_icons_init(void) {
    (void)ui_icon_set_load(&system_set);
}

const gfx_image_t*
ui_icon(ui_icon_id_t id) {
    return ui_icon_set_get(&system_set, (int)id);
}

void
ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color) {
    ui_icon_set_draw(ctx, r, &system_set, (int)id, color);
}
