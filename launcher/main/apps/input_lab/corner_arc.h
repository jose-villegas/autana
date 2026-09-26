/*
 * corner_arc - the shape of a rounded panel corner: how far in from the side
 * edge the glass starts on a given row, for a corner of radius `radius`.
 * Drawn as an arc and matched by eye to the visible edge, it measures the
 * radius; once measured, the same inset is the safe area for anything laid
 * out near a corner.
 */
#pragma once

/* Pixels hidden at the start of row `row` counted from the corner's edge:
 * `radius` at row 0, falling to 0 from row `radius` on. */
int corner_arc_inset(int radius, int row);
