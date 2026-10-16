/* Internal widget lifecycle; screen-facing wrappers live in ui_mu.h. */
#pragma once

#include "ui/ui_mu.h"

/* Forgets any open dropdown list's state; ui_init() calls it. */
void ui_widgets_reset(void);
