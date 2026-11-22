/*
 * render_settle: quiet bands refine to finest detail with texture shortcuts and small fill off.
 */
#pragma once

#include "render/partial/render_partial.h"

#define RENDER_SETTLE_BANDS               32
#define RENDER_SETTLE_COST_WEIGHT_DIVISOR 8
#define RENDER_SETTLE_COST_FRAC_BITS      3
#define RENDER_SETTLE_DIVIDE_PX           1

typedef struct {
    uint8_t quiet[RENDER_SETTLE_BANDS]; /* consecutive held frames whose live rows missed this band */
    bool refined[RENDER_SETTLE_BANDS];
    /* Microseconds per refined row with RENDER_SETTLE_COST_FRAC_BITS fractional bits, 0 until measured. */
    int32_t row_us;
} render_settle_t;

void render_settle_forget(render_settle_t* settle);
void render_settle_age(render_settle_t* settle, int height, render_rows_t drawn);
render_rows_t render_settle_next(const render_settle_t* settle, int height, render_settle_settings_t settings);
void render_settle_done(render_settle_t* settle, int height, render_rows_t rows, int32_t cost_us);
render_settings_t render_settle_quality(const render_settings_t* live);
bool render_settle_refines(const render_settings_t* live);
