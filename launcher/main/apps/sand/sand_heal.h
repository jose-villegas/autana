/*
 * sand_heal - sand's policy for gfx_heal_mark(): which screen rows to heal,
 * and when.
 *
 * Corruption on a fast panel link lands where sends happen, and sand sends
 * where grains move. So a band of rows that moved is healed once it has been
 * quiet for SAND_HEAL_SETTLE_FRAMES presents - the last send there is the
 * one worth correcting - and a band that keeps moving is healed after
 * SAND_HEAL_BUSY_FRAMES presents of it, since its unchanging pixels would
 * otherwise keep a stray one for the whole pour. No band heals twice within
 * SAND_HEAL_MIN_GAP_FRAMES. Bands nothing sent to are never healed; a blind
 * sweep costs as much on a still screen as on a pour.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SAND_HEAL_BAND_ROWS      8
#define SAND_HEAL_MAX_BANDS      64
#define SAND_HEAL_SETTLE_FRAMES  3
#define SAND_HEAL_BUSY_FRAMES    60

/* Measured on a host replay of settled water, whose shimmer sends a few
 * cells in many bands: at 24 the heal cost more than the shimmer itself. */
#define SAND_HEAL_MIN_GAP_FRAMES 60

/* Bands one step can return, each a separate run of healed bands. */
#define SAND_HEAL_MAX_SPANS      (SAND_HEAL_MAX_BANDS / 2)

typedef struct {
    uint64_t moved;                          /* bands sent to since the last step */
    uint8_t quiet[SAND_HEAL_MAX_BANDS];      /* presents since the band last moved */
    uint8_t busy[SAND_HEAL_MAX_BANDS];       /* presents it moved in since it last healed */
    uint8_t since_heal[SAND_HEAL_MAX_BANDS]; /* presents since it last healed */
    int rows;
    int bands;
} sand_heal_t;

typedef struct {
    int y0, y1;
} sand_heal_span_t;

void sand_heal_init(sand_heal_t* h, int screen_rows);

/* Screen rows [y0, y1) were sent this present. */
void sand_heal_note_rows(sand_heal_t* h, int y0, int y1);

/* Ends a present: advances every band and writes the row spans due for
 * healing to `out`, adjacent bands merged. Returns how many. */
int sand_heal_step(sand_heal_t* h, sand_heal_span_t* out, int max);
