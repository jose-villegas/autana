/*
 * gfx_double_buffer - buffer ownership for a full-redraw double-buffer
 * frame. The front buffer belongs to present; the other belongs to drawing.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    unsigned front;
    bool primed;
} gfx_double_buffer_t;

static inline void
gfx_double_buffer_begin(gfx_double_buffer_t* state) {
    state->front = 0;
    state->primed = false;
}

static inline unsigned
gfx_double_buffer_present_index(const gfx_double_buffer_t* state) {
    return state->front;
}

static inline unsigned
gfx_double_buffer_draw_index(const gfx_double_buffer_t* state) {
    return state->front ^ 1u;
}

static inline bool
gfx_double_buffer_primed(const gfx_double_buffer_t* state) {
    return state->primed;
}

static inline void
gfx_double_buffer_swap(gfx_double_buffer_t* state) {
    state->front ^= 1u;
    state->primed = true;
}
