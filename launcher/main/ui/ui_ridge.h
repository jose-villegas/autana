#pragma once

#include <stdint.h>

#include "app.h"

/* Between ui_begin() and ui_end_over(): turns this frame's touch into
 * plucks, moves the line on, and redraws the columns that moved. Costs
 * nothing while the line is at rest and untouched. */
void ui_ridge_step(const input_t* input, uint32_t dt_ms);

/* The whole backdrop, black and the ridge as it stands - a ui_backdrop_fn. */
void ui_ridge_paint(void);
