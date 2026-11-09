#include "scene/scene_shell.h"
#include "core/checkpoint.h"
#include "scene/scene_internal.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "render/context/render_context.h"

void
scene_shell_render(uint32_t dt_ms) {
    scene_render(dt_ms, display_quarter_now(), GFX_WIDTH, GFX_HEIGHT);
}

/* Where the frame composes: the framebuffer, and the half picture and its
 * seams unless an overlay or the filter has no use for them. Not inlined: a
 * leaf off the compose chain's deepest frames. */
static __attribute__((noinline)) void
fill_target(scene_target_t* target) {
    gfx_color_t* full = gfx_mode_has_framebuffer(gfx_mode_current()) ? gfx_framebuffer() : NULL;
    const bool overlay = render_context_has_overlay(render_context_main());
    const bool blended = render_context_upscale_filter(render_context_main()) == UPSCALE_FILTER_SURFACE;
    *target = (scene_target_t){full, GFX_WIDTH, GFX_HEIGHT, overlay ? NULL : gfx_half_picture(),
                               overlay || !blended ? NULL : gfx_half_seams()};
}

/* What follows a compose: the rows it rewrote expanded to the screen or, with
 * no expansion but a framebuffer, handed to the UI to paint over. Not
 * inlined, for the same reason. */
static __attribute__((noinline)) void
finish_compose(bool expanded, const scene_target_t* target) {
    const int first = render_context_rows_first(render_context_main());
    const int end = render_context_rows_end(render_context_main());
    if (expanded) {
        const bool blended = render_context_upscale_filter(render_context_main()) == UPSCALE_FILTER_SURFACE;
        gfx_expand_frame(blended, first, end);
    } else if (target->pixels != NULL) {
        gfx_mark_dirty(0, first, GFX_WIDTH, end - first);
        render_context_overlay(render_context_main(), GFX_WIDTH, GFX_HEIGHT);
        /* The UI paints over this framebuffer: the next compose writes every row. */
        (void)render_context_compose(render_context_main(), NULL, NULL, NULL);
    }
}

void
scene_shell_compose(uint32_t dt_ms) {
    if (!scene_has_active_camera()) {
        return;
    }
    scene_target_t target;
    fill_target(&target);
    /* Partial redraw rewrites some rows only; otherwise these are all of them. */
    finish_compose(scene_compose(dt_ms, display_quarter_now(), &target), &target);
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
scene_shell_checkpoint_capture(checkpoint_t* saved) {
    scene_draw_checkpoint_capture(saved);
    render_context_checkpoint_capture(saved, render_context_main());
}

bool
scene_shell_checkpoint_prepare(checkpoint_t* saved) {
    return render_context_checkpoint_prepare(saved, render_context_main());
}
#endif
