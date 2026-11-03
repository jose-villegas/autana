#include "ui/controls/ui_icons.h"

#include <stddef.h>

#include "draw/draw_image.h"

#include "asset/asset_store.h"
#include "draw/draw_image_format.h"
#include "esp_log.h"
#include "ui/controls/ui_widgets.h"
#include "ui/ui.h"

static const char* TAG = "ui_icons";

static const ui_icon_name_t SYSTEM_NAMES[UI_ICON_COUNT] = {
    [UI_ICON_CHECK] = {"check", "x"},
    [UI_ICON_CLOSE] = {"close", "X"},
    [UI_ICON_INFO] = {"info", "i"},
    [UI_ICON_ALERT] = {"alert", "!"},
    [UI_ICON_CHEVRON_LEFT] = {"chevron_left", "<"},
    [UI_ICON_CHEVRON_UP] = {"chevron_up", "^"},
    [UI_ICON_CHEVRON_DOWN] = {"chevron_down", "v"},
    [UI_ICON_ARROW_LEFT] = {"arrow_left", "<"},
    [UI_ICON_CHECKBOX_ON] = {"checkbox_on", "x"},
    [UI_ICON_HOME] = {"home", "H"},
    [UI_ICON_WIFI] = {"wifi", "W"},
    [UI_ICON_GAMEPAD] = {"gamepad", "G"},
    [UI_ICON_MONITOR] = {"monitor", "M"},
};

static image_t system_icons[UI_ICON_COUNT];
static ui_icon_set_t system_set = {ASSET_ENGINE_PACK, SYSTEM_NAMES, UI_ICON_COUNT, system_icons, false, HANDLE_NONE};

bool
ui_icon_set_load(ui_icon_set_t* set) {
    if (set->loaded) {
        return true;
    }
    /* A missing pack is logged by the store. */
    set->load = asset_store_load(set->pack);
    const asset_pack_t* pack = asset_store_loaded(set->load);
    if (pack == NULL) {
        asset_store_release(set->load);
        return false;
    }
    for (int i = 0; i < set->count; i++) {
        const char* name = set->names[i].name;
        asset_view_t entry;
        asset_status_t status = asset_pack_find(pack, name, DRAW_IMAGE_ASSET, &entry);
        if (status == ASSET_OK) {
            status = draw_image_open(entry, &set->icons[i]);
        }
        if (status == ASSET_OK && set->icons[i].format != IMAGE_MONO1) {
            status = ASSET_ERR_FORMAT;
        }
        if (status != ASSET_OK) {
            ESP_LOGW(TAG, "icon %s/%s: %s: the set is drawn as text", set->pack, name, asset_status_text(status));
            asset_store_release(set->load);
            return false;
        }
    }
    set->loaded = true;
    return true;
}

void
ui_icon_set_release(ui_icon_set_t* set) {
    if (set->loaded) {
        asset_store_release(set->load);
        set->loaded = false;
    }
}

const image_t*
ui_icon_set_get(const ui_icon_set_t* set, int i) {
    return set->loaded ? &set->icons[i] : NULL;
}

void
ui_icons_init(void) {
    (void)ui_icon_set_load(&system_set);
}

const image_t*
ui_icon_image(ui_icon_id_t id) {
    return ui_icon_set_get(&system_set, (int)id);
}

void
ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color) {
    ui_draw_icon(ctx, r, ui_icon_image(id), SYSTEM_NAMES[id].text, color);
}
