/* Internal widget lifecycle. */
#pragma once

#include "ui/controls/ui_icons.h"
#include "ui/ui_internal.h"

/* System icon `id` filling `r`, or its short text when there is no pack. */
void ui_draw_system_icon(mu_Context* ctx, mu_Rect r, ui_icon_id_t id, mu_Color color);

/* Forgets any open dropdown list's state; ui_init() calls it. */
void ui_widgets_reset(void);
