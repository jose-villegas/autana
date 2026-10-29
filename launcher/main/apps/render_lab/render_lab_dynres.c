#include "render_lab_dynres.h"

#include <stdio.h>

#include "gfx/gfx.h"
#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_view.h"

const resolution_step_t render_lab_ladder[RENDER_LAB_LADDER_STEPS] = {
    {368, 448}, {368, 358}, {368, 298}, {368, 224}, {184, 224}, {184, 179}, {184, 149}, {184, 112},
};

const resolution_model_t render_lab_ladder_model = {
    4427.0F, 1.3189F, 1.8201F, 35694.0F, {10181, 9137, 8246, 7261, 2611, 2269, 2035, 1759}};

/* The budget and scale last handed to the context; a budget of -1 for none
 * since the last stop. */
static int applied_budget_ms = -1;
static int applied_scale = -1;

#if TUNE_ENABLED
bool
render_lab_dynres_on(void) {
    return render_lab_budget_ms() != 0;
}

bool
render_lab_dynres_set_on(bool on) {
    return render_lab_budget_set_ms(on ? RENDER_LAB_TARGET_MS : 0);
}
#endif

void
render_lab_dynres_apply(void) {
    const int budget_ms = render_lab_budget_ms();
    const int scale = render_lab_scale();
    if (budget_ms == applied_budget_ms && scale == applied_scale) {
        return;
    }
    applied_budget_ms = budget_ms;
    applied_scale = scale;
    if (budget_ms == 0) {
        render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
        return;
    }
    resolution_config_t config = resolution_config(render_lab_ladder, RENDER_LAB_LADDER_STEPS, RENDER_LAB_LADDER_STEPS,
                                                   budget_ms * (int32_t)MATH_US_PER_MS);
    const int top = resolution_config_top(&config, render_lab_render_size(GFX_WIDTH, GFX_HEIGHT));
    render_context_set_dynamic_resolution(render_context_main(), &config, &render_lab_ladder_model, top);
}

void
render_lab_dynres_stop(void) {
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    applied_budget_ms = -1;
    applied_scale = -1;
}

const char*
render_lab_dynres_size(void) {
    static char size[RENDER_LAB_STATUS_LEN];
    const render_context_frame_t frame = render_context_frame(render_context_main());
    if (frame.step < 0 || snprintf(size, sizeof size, "%dx%d ", frame.width, frame.height) < 0) {
        size[0] = '\0';
    }
    return size;
}
