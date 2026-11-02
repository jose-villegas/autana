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

void
scene_shell_compose(uint32_t dt_ms) {
    if (!scene_has_active_camera()) {
        return;
    }
    gfx_color_t* full = gfx_mode_has_framebuffer(gfx_mode_current()) ? gfx_framebuffer() : NULL;
    const bool overlay = render_context_has_overlay(render_context_main());
    const upscale_filter_t filter = render_context_upscale_filter(render_context_main());
    const gfx_expand_t expand = filter == UPSCALE_FILTER_SURFACE ? GFX_EXPAND_BLENDED
                                : filter == UPSCALE_FILTER_EDGES ? GFX_EXPAND_CORNERS
                                                                 : GFX_EXPAND_NEAREST;
    const scene_target_t target = {full, GFX_WIDTH, GFX_HEIGHT, overlay ? NULL : gfx_half_picture(),
                                   overlay || expand == GFX_EXPAND_NEAREST ? NULL : gfx_half_hints()};
    if (scene_compose(dt_ms, display_quarter_now(), &target)) {
        gfx_expand_frame_filtered(expand);
    } else if (target.pixels != NULL) {
        gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
        render_context_overlay(render_context_main(), GFX_WIDTH, GFX_HEIGHT);
    }
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
