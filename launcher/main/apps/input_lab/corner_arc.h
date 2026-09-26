/*
 * corner_arc - the shape of a rounded panel corner: how far in from the side
 * edge the glass starts on a given row, for a corner of radius `radius`.
 */
#pragma once

/* Pixels hidden at the start of row `row` counted from the corner's edge:
 * `radius` at row 0, falling to 0 from row `radius` on. */
int corner_arc_inset(int radius, int row);
