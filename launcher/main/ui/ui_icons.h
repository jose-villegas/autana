/*
 * ui_icons: icons from a pack, looked up by the names their owner's code
 * lists, and the system icons every screen means the same way, from the
 * engine pack. Without a pack nothing is drawn from nothing: an icon beside
 * a label leaves the label alone, and an icon-only control shows its icon's
 * short text in the icon's box.
 */
#pragma once

#include <stdbool.h>

#include "gfx/image/gfx_image.h"
#include "microui.h"

/* The pack every app may read engine artwork from: loaded by ui_init(),
 * before an app can hold store slots, and kept for the run. */
#define UI_ENGINE_PACK "engine"

/* One icon: its entry's name in the pack, and what an icon-only control
 * shows in its place without one. */
typedef struct {
    const char* name;
    const char* text;
} ui_icon_name_t;

/* Icons an owner reads from one pack: `count` names in, `count` images out.
 * Loaded, it holds one use of the pack. */
typedef struct {
    const char* pack;
    const ui_icon_name_t* names;
    int count;
    gfx_image_t* icons; /* `count` of them */
    bool loaded;
} ui_icon_set_t;

/* Mounts the set's pack and finds every icon, each a GFX_IMAGE_MONO1 image
 * entry. False when the pack or any icon is missing or not one bit, with one
 * log line; nothing is held then. Loading a loaded set does nothing. */
bool ui_icon_set_load(ui_icon_set_t* set);

/* Drops the pack use a loaded set holds; every icon is NULL after it. */
void ui_icon_set_release(ui_icon_set_t* set);

/* Icon `i`, or NULL unless the set is loaded. */
const gfx_image_t* ui_icon_set_get(const ui_icon_set_t* set, int i);

/* Icon `i` filling `r`, or its text when the set is not loaded. */
void ui_icon_set_draw(mu_Context* ctx, mu_Rect r, const ui_icon_set_t* set, int i, mu_Color color);

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

/* Loads the system icons; ui_init() calls it. */
void ui_icons_init(void);

/* System icon `id`, or NULL when the engine pack does not hold it. */
const gfx_image_t* ui_icon(ui_icon_id_t id);

/* System icon `id` filling `r`, or its short text when there is no pack. */
void ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color);
