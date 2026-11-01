/* Checkpoint replay of the animated scene through every declared debug view. */
#include <stdlib.h>
#include "../scene_viewer.c"
#include "core/tune.h"
#include "gfx/present/gfx_present.h"

#define CHECKPOINT_STEP_MS               37
#define CHECKPOINT_AT                    5
#define CHECKPOINT_END                   12
#define CHECKPOINT_DIVERGE               9
#define CHECKPOINT_DIVERGE_SCALE_PERCENT (RENDER_CONTEXT_DEFAULT_SCALE_PERCENT / 2)

static provider_frame_t observed;

static void
observe_frame(const provider_frame_t* frame) {
    observed = *frame;
    draw(frame);
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
    if (!render_host_open(&provider, argc, argv, &code)) {
        return code;
    }
    size_t bytes;
    (void)render_host_pixels(&bytes);
    uint8_t* expected = malloc(bytes);
    if (expected == NULL) {
        return 1;
    }
    checkpoint_t lifetime = {0};
    void* allocation = NULL;
    checkpoint_capture_at(&lifetime, &allocation, bytes);
    allocation = expected;
    if (checkpoint_restore(&lifetime)) {
        fprintf(stderr, "checkpoint: changed allocation accepted\n");
        return 1;
    }
    checkpoint_clear(&lifetime);
    bool ok = true;
    for (int v = RENDER_DEBUG_VIEW_SHADED; v <= RENDER_DEBUG_VIEW_COUNT; v++) {
        const render_debug_view_t* row = render_context_debug_view(v);
        const char* name = row == NULL ? "shaded" : row->name;
        render_context_set_debug_view(render_context_main(), v);
        render_context_set_scale(render_context_main(), RENDER_CONTEXT_DEFAULT_SCALE_PERCENT);
        gfx_set_interlace(false);
        render_context_set_frame_generation(render_context_main(), false);
        render_context_set_dynamic_resolution(render_context_main(), NULL, NULL, 0);
        step_frames(1);
        render_host_checkpoint_t* reset = render_host_save();
        if (reset == NULL) {
            return 1;
        }
        step_frames(CHECKPOINT_END);
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
        const tune_entry_t* entry = tune_find(tune_shared(), TUNE_NAME(render, cull));
        const int32_t value = *entry->value;
        const uint32_t generation = entry->owner->generation;
        (void)tune_set(tune_shared(), entry->name, value == entry->low ? entry->high : entry->low);
        render_context_set_scale(render_context_main(), CHECKPOINT_DIVERGE_SCALE_PERCENT);
        gfx_set_interlace(true);
        step_frames(CHECKPOINT_DIVERGE);
        if (memcmp(expected, render_host_pixels(&bytes), bytes) == 0) {
            fprintf(stderr, "checkpoint %s: divergence FAIL\n", name);
            ok = false;
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
        step_frames(CHECKPOINT_END - CHECKPOINT_AT);
        const bool same = memcmp(expected, render_host_pixels(&bytes), bytes) == 0
                          && render_context_main()->scale_percent == RENDER_CONTEXT_DEFAULT_SCALE_PERCENT
                          && observed.index == expected_frame.index && observed.elapsed_ms == expected_frame.elapsed_ms;
        fprintf(stderr, "checkpoint %s: %s\n", name, same ? "PASS" : "FAIL");
        ok &= same;
        render_host_checkpoint_free(saved);
        render_host_checkpoint_free(reset);
    }
    free(expected);
    return render_host_close() && ok ? 0 : 1;
}
