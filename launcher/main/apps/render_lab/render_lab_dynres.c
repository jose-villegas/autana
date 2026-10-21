#include "render_lab_dynres.h"

#include <stdio.h>

#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_view.h"

const resolution_step_t render_lab_ladder[RENDER_LAB_LADDER_STEPS] = {
    {368, 448}, {368, 358}, {368, 298}, {368, 224}, {184, 224}, {184, 179}, {184, 149},
};

const resolution_model_t render_lab_ladder_model = {
    7852.0F, 1.4724F, 2.7419F, 52171.0F, {10376, 9365, 8252, 7304, 2752, 5575, 5428}};

/* The budget last handed to the context; -1 for none since the last stop. */
static int applied_budget_ms = -1;

void
render_lab_dynres_apply(void) {
    const int budget_ms = render_lab_budget_ms();
    if (budget_ms == applied_budget_ms) {
        return;
    }
    applied_budget_ms = budget_ms;
    if (budget_ms == 0) {
        render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
        return;
    }
    const resolution_config_t config = resolution_config(render_lab_ladder, RENDER_LAB_LADDER_STEPS,
                                                         RENDER_LAB_LADDER_RECOVERY, budget_ms * MATH_US_PER_MS);
    render_context_set_dynamic_resolution(render_context_main(), &config, &render_lab_ladder_model,
                                          RENDER_LAB_LADDER_HALF);
}

void
render_lab_dynres_stop(void) {
    render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
    applied_budget_ms = -1;
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
