/*
 * ui_pointer: input_t to a short list of pointer events, held not tapped.
 *
 * Pure logic, no microui and no gfx; ui_bridge.c talks to microui. Coordinates
 * are LOGICAL, the UI's own: the caller maps a touch off the panel first.
 */
#pragma once

#include "math/linear/vec2i.h"

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"

typedef enum {
    UI_POINTER_MOVE,
    UI_POINTER_DOWN,
    UI_POINTER_UP,
    UI_POINTER_SCROLL,
} ui_pointer_kind_t;

/* For UI_POINTER_SCROLL, point is the content's scroll delta in logical
 * pixels: the opposite of the finger's own movement. */
typedef struct {
    ui_pointer_kind_t kind;
    vec2i_t point;
} ui_pointer_event_t;

#define UI_POINTER_NOWHERE        ((vec2i_t){-1, -1})

/* A frame never needs more than an owed UP, then a move, a down and an up
 * for a tap that resolves within it. */
#define UI_POINTER_MAX_EVENTS     4

/* How far a finger on scrollable content moves before it is a drag rather
 * than a tap. microui controls act on the press, so on such content the DOWN
 * waits for the release, or for a sideways move a slider needs. */
#define UI_POINTER_DRAG_THRESHOLD 12

typedef struct {
    /* The DOWN goes out on the frame of contact, aimed at the hover root the
     * caller seeded from the last frame. Set until the caller reports whether
     * it landed; a miss is pressed again on the next frame. A press and
     * release inside one frame resolves at once, so a miss there is lost. */
    bool press_unconfirmed;
    vec2i_t press;
    vec2i_t aim;
    bool aimed;
    bool down; /* a DOWN went out with no matching UP yet */

    /* Set by the caller before a press frame and after every frame: whether
     * the pointer rests on content that can scroll. */
    bool over_scrollable;

    /* Set by the caller after every frame: the seeded hover root was not the
     * one the frame found, and nothing took focus. */
    bool press_missed;
    /* A press on scrollable content waiting for the lift, a sideways move or
     * a scroll; the control under it draws pressed meanwhile. */
    bool press_deferred;
    bool dragging;
    vec2i_t last;
} ui_pointer_t;

/* Aim the next synthesized press at a control without moving its raw drag
 * origin. */
void ui_pointer_aim(ui_pointer_t* p, vec2i_t point);

/* Feed one frame's input_t; get back 0-UI_POINTER_MAX_EVENTS events in `out`,
 * in playback order. Returns the count written, or 0 if `max` can't hold the
 * largest possible result, ui_bezel_spans()'s all-or-nothing rule, so a
 * partial count never lets a caller read past a too-small array. */
int ui_pointer_step(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out, int max);
