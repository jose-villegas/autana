/* Checkpoint replay of the animated scene through every declared debug view. */
#include <stdlib.h>
#include "../scene_viewer.c"
#include "core/tune.h"
#include "gfx/present/gfx_present.h"

#define CHECKPOINT_STEP_MS 37
#define CHECKPOINT_AT      5
#define CHECKPOINT_END     12
#define CHECKPOINT_DIVERGE 9

static provider_frame_t observed;

static void
observe_frame(const provider_frame_t* frame) {
    observed = *frame;
    draw(frame);
}

static int
step_frames(int count) {
    const input_t input = {0};
    int generated = 0;
    for (int i = 0; i < count; i++) {
        render_host_step(CHECKPOINT_STEP_MS, &input);
        generated += render_context_frame(render_context_main()).generated;
    }
    return generated;
}

static void
read_picture(gfx_color_t* pixels) {
    for (int y = 0; y < GFX_HEIGHT; y++) {
        gfx_read_panel_row(y, pixels + (size_t)y * GFX_WIDTH);
    }
}

int
main(int argc, char** argv) {
    int code;
    frame_provider_t provider = scene_provider;
    provider.draw = observe_frame;
    if (!render_host_open(&provider, argc, argv, &code)) {
        return code;
    }
    gfx_color_t* expected = malloc(GFX_FRAMEBUFFER_BYTES);
    gfx_color_t* actual = malloc(GFX_FRAMEBUFFER_BYTES);
    render_host_checkpoint_t* start = render_host_save();
    if (expected == NULL || actual == NULL || start == NULL) {
        return 1;
    }
    bool ok = true;
    for (int v = RENDER_DEBUG_VIEW_SHADED; v <= RENDER_DEBUG_VIEW_COUNT; v++) {
        const render_debug_view_t* row = render_context_debug_view(v);
        const char* name = row == NULL ? "shaded" : row->name;
        render_host_restore(start);
        render_context_set_debug_view(render_context_main(), v);
        render_context_set_frame_generation(render_context_main(), true);
        render_host_checkpoint_t* reset = render_host_save();
        if (reset == NULL) {
            return 1;
        }
        const int generated = step_frames(CHECKPOINT_END);
        if (v == RENDER_DEBUG_VIEW_SHADED && generated == 0) {
            fprintf(stderr, "checkpoint %s: no generated frame exercised\n", name);
            ok = false;
        }
        read_picture(expected);
        render_host_restore(reset);
        step_frames(CHECKPOINT_AT);
        render_host_checkpoint_t* saved = render_host_save();
        if (saved == NULL) {
            return 1;
        }
        const tune_entry_t* entry = tune_shared()->first;
        const int32_t value = *entry->value;
        const uint32_t generation = entry->owner->generation;
        (void)tune_set(tune_shared(), entry->name, value == entry->low ? entry->high : entry->low);
        step_frames(CHECKPOINT_DIVERGE);
        render_host_restore(saved);
        if (*entry->value != value || entry->owner->generation != generation) {
            fprintf(stderr, "checkpoint %s: tunable state FAIL\n", name);
            ok = false;
        }
        step_frames(CHECKPOINT_END - CHECKPOINT_AT);
        read_picture(actual);
        const bool same = memcmp(expected, actual, GFX_FRAMEBUFFER_BYTES) == 0 && observed.index == CHECKPOINT_END - 1
                          && observed.elapsed_ms == (CHECKPOINT_END - 1) * CHECKPOINT_STEP_MS;
        fprintf(stderr, "checkpoint %s: %s\n", name, same ? "PASS" : "FAIL");
        ok &= same;
        render_host_checkpoint_free(saved);
        render_host_checkpoint_free(reset);
    }
    render_host_checkpoint_free(start);
    free(actual);
    free(expected);
    return render_host_close() && ok ? 0 : 1;
}
