/*
 * render_settle: quiet bands of a held picture refine with live texture
 * shortcuts and small fill off. Live rows reset the bands they touch;
 * middle-out runs spend a budget learned from measured row costs.
 */
#pragma once

#include "render/partial/render_partial.h"

#define RENDER_SETTLE_BANDS               32
#define RENDER_SETTLE_COST_WEIGHT_DIVISOR 8
#define RENDER_SETTLE_DIVIDE_PX           1

typedef struct {
    uint8_t quiet[RENDER_SETTLE_BANDS];
    bool refined[RENDER_SETTLE_BANDS];
    int32_t row_us;
} render_settle_t;

void render_settle_forget(render_settle_t* settle);
void render_settle_age(render_settle_t* settle, int height, render_rows_t drawn);
render_rows_t render_settle_next(const render_settle_t* settle, int height, render_settle_settings_t settings);
void render_settle_done(render_settle_t* settle, int height, render_rows_t rows, int32_t cost_us);
render_settings_t render_settle_quality(const render_settings_t* live);
bool render_settle_refines(const render_settings_t* live);
