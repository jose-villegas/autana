/*
 * rect_overlap: overlaps within a list of recti, found by one sort and sweep,
 * and what is left of a rectangle once another is taken out of it.
 */
#pragma once

#include <stdbool.h>

#include "math/geometry/recti.h"

/* recti_subtract() leaves at most this many pieces. */
#define RECTI_SUBTRACT_MAX 4

/* Sorts `r` by its start along x when `across`, else along y, and sweeps it:
 * the first i whose rect overlaps a later one, that one's index in *j, or -1
 * when none overlap. A rect can overlap only those starting before it ends
 * along the axis, so a list thin along it sweeps in about count steps. */
int recti_overlapping_pair(recti_t* r, int count, bool across, int* j);

/* The pieces of `a` outside `b`, disjoint, written to `out`; how many. A `b`
 * reaching past `a` on any side leaves at most RECTI_SUBTRACT_MAX - 1. */
int recti_subtract(recti_t a, recti_t b, recti_t out[RECTI_SUBTRACT_MAX]);
