#include "apps/render_lab/render_lab_view.h"

#include "services/tune.h"

TUNE_OWNER(render_lab);
TUNE(render_lab, view, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_COUNT);
TUNE(render_lab, scale, RENDER_LAB_SCALE_DEFAULT, RENDER_LAB_SCALE_UNIT, RENDER_LAB_SCALE_MAX);
TUNE(render_lab, present, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_DOUBLE);
TUNE(render_lab, budget, 0, 0, 200);

int
render_lab_debug_view(void) {
    return (int)view;
}

int
render_lab_scale(void) {
    return scale;
}

camera_present_t
render_lab_present(void) {
    return (camera_present_t)present;
}

resolution_step_t
render_lab_render_size(int width, int height) {
    const int percent = RENDER_LAB_SCALE_UNIT * RENDER_LAB_SCALE_UNIT / render_lab_scale();
    return raster_present_size(
        (resolution_step_t){width * percent / RENDER_LAB_SCALE_UNIT, height * percent / RENDER_LAB_SCALE_UNIT},
        render_lab_present());
}

void
render_lab_apply_raster_settings(void) {
    render_context_set_scale(render_context_main(), RENDER_LAB_SCALE_UNIT * RENDER_LAB_SCALE_UNIT / render_lab_scale());
}

int
render_lab_budget_ms(void) {
    return budget;
}
