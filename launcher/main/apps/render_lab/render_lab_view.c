#include "apps/render_lab/render_lab_view.h"

#include "services/tune.h"

TUNE_OWNER(render_lab);
TUNE(render_lab, view, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_COUNT);
TUNE(render_lab, scale, RENDER_LAB_SCALE_DEFAULT, RENDER_LAB_SCALE_UNIT, RENDER_LAB_SCALE_MAX);
TUNE(render_lab, rows, 0, 0, RENDER_LAB_SCALE_MAX);
TUNE(render_lab, scanline, 0, 0, UPSCALE_SCANLINE_MAX);
TUNE(render_lab, budget, 0, 0, 200);

int
render_lab_debug_view(void) {
    return (int)view;
}

int
render_lab_scale(void) {
    return scale;
}

int
render_lab_rows(void) {
    return rows == 0 ? scale : (rows < RENDER_LAB_SCALE_UNIT ? RENDER_LAB_SCALE_UNIT : rows);
}

int
render_lab_scanline(void) {
    return scanline;
}

resolution_step_t
render_lab_render_percent(void) {
    const int width_percent = RENDER_LAB_SCALE_UNIT * RENDER_LAB_SCALE_UNIT / render_lab_scale();
    const int height_percent = RENDER_LAB_SCALE_UNIT * RENDER_LAB_SCALE_UNIT / render_lab_rows();
    return (resolution_step_t){width_percent, height_percent};
}

resolution_step_t
render_lab_render_size(int width, int height) {
    const resolution_step_t percent = render_lab_render_percent();
    return (resolution_step_t){width * percent.width / RENDER_LAB_SCALE_UNIT,
                               height * percent.height / RENDER_LAB_SCALE_UNIT};
}

void
render_lab_apply_raster_settings(void) {
    const resolution_step_t percent = render_lab_render_percent();
    render_context_set_scale(render_context_main(), percent.width, percent.height);
    render_context_set_scanlines(render_context_main(), render_lab_scanline());
}

int
render_lab_budget_ms(void) {
    return budget;
}
