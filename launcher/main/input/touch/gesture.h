/*
 * gesture: the home-edge zone and swipe, read from input state.
 *
 * Pure logic, testable on the host: screen size and the edge carrying the
 * home gesture are parameters. Which edge that is, given the board's
 * rotation, is the shell's knowledge, not this module's.
 */
#pragma once

#include <stdbool.h>

#include "input/input.h"

/* Which physical edge of the screen the home gesture currently lives on.
 * The caller decides this (see shell_exit_edge_for_quarter()) by
 * mapping the logical canvas's bottom edge to the physical panel at the
 * current quarter turn. */
typedef enum {
    GESTURE_EDGE_TOP,
    GESTURE_EDGE_BOTTOM,
    GESTURE_EDGE_LEFT,
    GESTURE_EDGE_RIGHT,
} gesture_edge_t;

/* How far into the screen from the target edge a home swipe may begin.
 * Generous, because a fingertip landing "at the edge" is not precise. Named
 * for depth, not height, since it applies to a left/right edge as much as a
 * top/bottom one. */
#define GESTURE_HOME_ZONE_DEPTH 64

/* How far it must travel away from that edge, toward the centre, to count.
 * Large enough that a tap wobbling near the edge cannot trigger it by
 * accident. */
#define GESTURE_HOME_SWIPE_DIST 90

/* True when (x, y) lies within GESTURE_HOME_ZONE_DEPTH of `edge`: where an
 * edge gesture may begin, so a touch landing there belongs to the shell. */
bool gesture_in_edge_zone(int x, int y, gesture_edge_t edge, int screen_w, int screen_h);

/* True while a finger that started near the given edge has travelled far
 * enough toward the centre of the screen.
 *
 * Requires the finger to still be down, so it fires partway through the swipe
 * rather than on release: waiting for the lift feels sluggish. That also means
 * it must not match on stale coordinates once contact ends. */
bool gesture_is_home_swipe(const input_t* input, gesture_edge_t edge, int screen_w, int screen_h);
