/* Frame generation with dynamic resolution: every second frame is warped from
 * the one drawn before it, and each drawn frame is priced for the pair. */
#include "render/context/render_context.h"

static void
framegen_example(render_context_t* context, const resolution_config_t* ladder) {
    render_context_set_dynamic_resolution(context, ladder, NULL, 0); /* ladder->budget_us: per shown frame */
    render_context_set_frame_generation(context, true);
}

/* After a frame: whether it was generated rather than drawn. */
static bool
framegen_example_generated(const render_context_t* context) {
    return render_context_frame(context).generated;
}
