#include "apps/render_lab/ui/render_lab_settings.h"

#include <stdbool.h>

#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "lab/ui/lab_menu_screen.h"
#include "ui/ui.h"

void
render_lab_settings_draw(int width) {
    int count = 0;
    const char* const* names = render_lab_small_fill_names(&count);
    int fill = render_lab_small_fill_selected();
    lab_menu_caption(width, RENDER_LAB_SMALL_FILL_LABEL);
    ui_row_centre(width, UI_ROW_HEIGHT);
    if (ui_dropdown(ui_next(), RENDER_LAB_SMALL_FILL_LABEL, &fill, names, count)) {
        (void)render_lab_small_fill_select(fill);
    }
    const char* const* upscales = render_lab_upscale_names(&count);
    int upscale = render_lab_upscale_selected();
    lab_menu_caption(width, RENDER_LAB_UPSCALE_LABEL);
    ui_row_centre(width, UI_ROW_HEIGHT);
    if (ui_dropdown(ui_next(), RENDER_LAB_UPSCALE_LABEL, &upscale, upscales, count)) {
        (void)render_lab_upscale_select(upscale);
    }
    bool dynamic = render_lab_dynres_on();
    ui_row_centre(width, UI_ROW_HEIGHT);
    if (ui_toggle(ui_next(), RENDER_LAB_DYNAMIC_RES_LABEL, &dynamic)) {
        (void)render_lab_dynres_set_on(dynamic);
    }
}
