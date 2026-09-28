/*
 * gfx_band_run - one band-frame loop, parameterised for host tests.
 *
 * The shell supplies the live gfx and UI operations; a host suite supplies
 * mocks and verifies the contract without a panel.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx_color.h"

typedef void (*gfx_band_draw_fn)(int row0, int row1, gfx_color_t* target);

typedef struct {
    void (*frame_begin)(void* context);
    bool (*next)(void* context);
    bool (*dirty)(void* context);
    int (*row0)(void* context);
    int (*height)(void* context);
    gfx_color_t* (*buffer)(void* context);
    void (*replay)(void* context, int row0, int row1);
    void (*submit)(void* context);
    void (*skip)(void* context);
} gfx_band_run_t;

static inline bool
gfx_band_run(const gfx_band_run_t* run, void* context, gfx_band_draw_fn draw) {
    if (draw == NULL) {
        return false;
    }

    run->frame_begin(context);
    while (run->next(context)) {
        if (!run->dirty(context)) {
            run->skip(context);
            continue;
        }
        const int row0 = run->row0(context);
        const int row1 = row0 + run->height(context);
        draw(row0, row1, run->buffer(context));
        run->replay(context, row0, row1);
        run->submit(context);
    }
    return true;
}
