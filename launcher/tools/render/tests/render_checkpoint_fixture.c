/* Checkpoint replay of the animated scene through every declared debug view. */
#include <stdlib.h>
#include "../scene_viewer.c"
#include "core/tune.h"
#include "draw/draw.h"
#include "gfx/present/gfx_present.h"

#define CHECKPOINT_STEP_MS               37
#define CHECKPOINT_AT                    5
#define CHECKPOINT_END                   12
#define CHECKPOINT_END_ODD               (CHECKPOINT_END + 1)
#define CHECKPOINT_DIVERGE_SCALE_PERCENT (RENDER_CONTEXT_DEFAULT_SCALE_PERCENT / 2)

#define CHECKPOINT_SWATCH_SIZE           8
#define CHECKPOINT_COLOUR_DEFAULT        0x204080
#define CHECKPOINT_COLOUR_DIVERGED       0xFF0040
#define CHECKPOINT_COLOUR_MAX            0xFFFFFF

TUNE_OWNER(checkpoint);
TUNE(checkpoint, colour, CHECKPOINT_COLOUR_DEFAULT, 0, CHECKPOINT_COLOUR_MAX);

static provider_frame_t observed;

static bool
checkpoint_setup(int quarter) {
    const gfx_mode_request_t request = {.layout = GFX_LAYOUT_DOUBLE_FB};
    return setup(quarter) && gfx_mode_enter(&request)->layout == request.layout;
}

static void
observe_frame(const provider_frame_t* frame) {
    observed = *frame;
    draw(frame);
    draw_rect(0, 0, CHECKPOINT_SWATCH_SIZE, CHECKPOINT_SWATCH_SIZE, RGB565BE((uint32_t)colour));
}

static void
step_frames(int count) {
    const input_t input = {0};
    for (int i = 0; i < count; i++) {
        render_host_step(CHECKPOINT_STEP_MS, &input);
    }
}

int
main(int argc, char** argv) {
    int code;
    frame_provider_t provider = scene_provider;
    provider.draw = observe_frame;
    provider.setup = checkpoint_setup;
    if (!render_host_open(&provider, argc, argv, &code)) {
        return code;
    }
    size_t bytes;
    (void)render_host_pixels(&bytes);
    uint8_t* expected = malloc(bytes);
    if (expected == NULL) {
        return 1;
    }

    bool ok = true;
    for (int end = CHECKPOINT_END; end <= CHECKPOINT_END_ODD; end++) {
        for (int v = RENDER_DEBUG_VIEW_SHADED; v <= RENDER_DEBUG_VIEW_COUNT; v++) {
            const render_debug_view_t* row = render_context_debug_view(v);
            const char* name = row == NULL ? "shaded" : row->name;
            render_context_set_debug_view(render_context_main(), v);
            render_context_set_scale(render_context_main(), RENDER_CONTEXT_DEFAULT_SCALE_PERCENT);
            (void)tune_set(tune_shared(), TUNE_NAME(checkpoint, colour), CHECKPOINT_COLOUR_DEFAULT);
            gfx_set_interlace(false);
            (void)gfx_set_clear_on_send(false, RGB565BE(0));
            render_context_set_frame_generation(render_context_main(), false);
            render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
            step_frames(1);
            render_host_checkpoint_t* reset = render_host_save();
            if (reset == NULL) {
                return 1;
            }
            step_frames(end);
            memcpy(expected, render_host_pixels(&bytes), bytes);
            const provider_frame_t expected_frame = observed;
            if (!render_host_restore(reset)) {
                fprintf(stderr, "checkpoint %s: reset refused\n", name);
                return 1;
            }
            step_frames(CHECKPOINT_AT);
            render_host_checkpoint_t* saved = render_host_save();
            if (saved == NULL) {
                return 1;
            }
            const tune_entry_t* entry = tune_find(tune_shared(), TUNE_NAME(checkpoint, colour));
            const int32_t value = *entry->value;
            const uint32_t generation = entry->owner->generation;
            (void)tune_set(tune_shared(), entry->name, CHECKPOINT_COLOUR_DIVERGED);
            render_context_set_scale(render_context_main(), CHECKPOINT_DIVERGE_SCALE_PERCENT);
            gfx_set_interlace(true);
            (void)gfx_set_clear_on_send(true, RGB565BE(0));
            step_frames(end - CHECKPOINT_AT);
            if (memcmp(expected, render_host_pixels(&bytes), bytes) == 0) {
                fprintf(stderr, "checkpoint %s: divergence FAIL\n", name);
                ok = false;
            }
            if (v == RENDER_DEBUG_VIEW_SHADED) {
                render_context_set_debug_view(render_context_main(), render_context_debug_view_named("motion"));
                step_frames(1);
            }
            if (!render_host_restore(saved)) {
                fprintf(stderr, "checkpoint %s: restore refused\n", name);
                return 1;
            }
            if (gfx_interlace_enabled()) {
                fprintf(stderr, "checkpoint %s: gfx state FAIL\n", name);
                ok = false;
            }
            if (*entry->value != value || entry->owner->generation != generation) {
                fprintf(stderr, "checkpoint %s: tunable state FAIL\n", name);
                ok = false;
            }
            step_frames(end - CHECKPOINT_AT);
            const bool pixels = memcmp(expected, render_host_pixels(&bytes), bytes) == 0;
            const bool same = render_context_main()->scale_percent == RENDER_CONTEXT_DEFAULT_SCALE_PERCENT
                              && observed.index == expected_frame.index
                              && observed.elapsed_ms == expected_frame.elapsed_ms;
            fprintf(stderr, "checkpoint %s end=%d: PIXELS %s state %s\n", name, end, pixels ? "PASS" : "FAIL",
                    same ? "PASS" : "FAIL");
            ok &= pixels && same;
            render_host_checkpoint_free(saved);
            render_host_checkpoint_free(reset);
        }
    }
    free(expected);
    return render_host_close() && ok ? 0 : 1;
}
