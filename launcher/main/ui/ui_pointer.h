/*
 * ui_pointer - input_t to a short list of pointer events, held not tapped.
 *
 * Pure logic, no microui and no gfx; ui_bridge.c talks to microui. Coordinates
 * are LOGICAL, the UI's own: the caller maps a touch off the panel first.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"

typedef enum {
    UI_POINTER_MOVE,
    UI_POINTER_DOWN,
    UI_POINTER_UP,
    UI_POINTER_SCROLL,
} ui_pointer_kind_t;

/* For UI_POINTER_SCROLL, x and y are how far to scroll the content, in
 * logical pixels: the opposite of the finger's own movement. */
typedef struct {
    ui_pointer_kind_t kind;
    int x, y;
} ui_pointer_event_t;

/* A frame never needs more than an owed UP, then a move, a down and an up
 * for a tap that resolves within it. */
#define UI_POINTER_MAX_EVENTS     4

/* How far a finger on scrollable content moves before it is a drag rather
 * than a tap. microui controls act on the press, so on such content the DOWN
 * waits for the release, or for a sideways move a slider needs. */
#define UI_POINTER_DRAG_THRESHOLD 12

typedef struct {
    /* Non-zero while a press is staged: focus needs hover and microui hovers
     * only with the button up, so the DOWN follows a MOVE-only frame once the
     * hover root is settled. A press and release inside one frame resolves at
     * once, so on an unsettled root it is lost. */
    uint8_t press_stage;
    int press_x, press_y;
    int aim_x, aim_y;
    bool aimed;
    bool down; /* a DOWN went out with no matching UP yet */

    /* Set by the caller after each frame: whether the pointer rests on
     * content that can scroll. */
    bool over_scrollable;

    /* Set by the caller after every frame: the root microui hovers is not
     * the one it found under the pointer, so no control can be hovered yet. */
    bool hover_unsettled;
    bool press_deferred;
    bool dragging;
    int last_x, last_y;
} ui_pointer_t;

/* Aim the next synthesized press at a control without moving its raw drag
 * origin. */
void ui_pointer_aim(ui_pointer_t* p, int x, int y);

/* Feed one frame's input_t; get back 0-UI_POINTER_MAX_EVENTS events in `out`,
 * in playback order. Returns the count written, or 0 if `max` can't hold the
 * largest possible result - ui_bezel_spans()'s all-or-nothing rule, so a
 * partial count never lets a caller read past a too-small array. */
int ui_pointer_step(ui_pointer_t* p, const input_t* input, ui_pointer_event_t* out, int max);
