/*
 * ui_icons: icons loaded from a pack by the names their owner's code lists,
 * and the system icons, from the engine pack, every screen means the same
 * way. Without a pack nothing is drawn from nothing: an icon beside a label
 * leaves the label alone, and an icon-only control shows a short text in the
 * icon's box (ui_draw_icon()).
 */
#pragma once

#include <stdbool.h>

#include "gfx/draw/icon.h"
#include "microui.h"

/* The pack every app may read engine artwork from, mounted on first use
 * and kept. */
#define UI_ENGINE_PACK "engine"

typedef enum {
    UI_ICON_CHECK,
    UI_ICON_CLOSE,
    UI_ICON_INFO,
    UI_ICON_ALERT,
    UI_ICON_CHEVRON_LEFT,
    UI_ICON_CHEVRON_RIGHT,
    UI_ICON_CHEVRON_UP,
    UI_ICON_CHEVRON_DOWN,
    UI_ICON_ARROW_LEFT,
    UI_ICON_CHECKBOX_ON,
    UI_ICON_HOME,
    UI_ICON_WIFI,
    UI_ICON_GAMEPAD,
    UI_ICON_MONITOR,
    UI_ICON_PLUS,
    UI_ICON_COUNT
} ui_icon_id_t;

/* Fills out[i] with icon names[i], a GFX_IMAGE_MONO1 image entry of pack
 * `pack`, and holds one use of the pack, which asset_store_release() drops.
 * False when the pack or any icon is missing or not an icon, with one log
 * line; nothing is held then, and the caller draws text where the icons
 * would be. */
bool ui_icons_load(const char* pack, const char* const* names, int count, gfx_image_t* out);

/* System icon `id`, or NULL when the engine pack does not hold it; the
 * first call loads them all. */
const gfx_image_t* ui_icon(ui_icon_id_t id);

/* The short text an icon-only control shows for system icon `id` when
 * ui_icon() has none. */
const char* ui_icon_text(ui_icon_id_t id);

/* System icon `id` filling `r`, or its text when there is no pack. */
void ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color);
