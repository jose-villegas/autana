#include "sand_heal.h"

#include <string.h>

void
sand_heal_init(sand_heal_t* h, int screen_rows) {
    memset(h, 0, sizeof *h);
    memset(h->since_heal, UINT8_MAX, sizeof h->since_heal);
    h->bands = (screen_rows + SAND_HEAL_BAND_ROWS - 1) / SAND_HEAL_BAND_ROWS;
    h->bands = h->bands > SAND_HEAL_MAX_BANDS ? SAND_HEAL_MAX_BANDS : h->bands;
    h->rows = screen_rows < h->bands * SAND_HEAL_BAND_ROWS ? screen_rows : h->bands * SAND_HEAL_BAND_ROWS;
}

void
sand_heal_note_rows(sand_heal_t* h, int y0, int y1) {
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 > h->rows ? h->rows : y1;
    for (int b = y0 / SAND_HEAL_BAND_ROWS; y0 < y1 && b <= (y1 - 1) / SAND_HEAL_BAND_ROWS; b++) {
        h->moved |= (uint64_t)1 << b;
    }
}

static uint8_t
saturating_increment(uint8_t v) {
    return v == UINT8_MAX ? v : (uint8_t)(v + 1);
}

static bool
band_due(sand_heal_t* h, int b) {
    h->since_heal[b] = saturating_increment(h->since_heal[b]);
    if ((h->moved >> b) & 1u) {
        h->quiet[b] = 0;
        h->busy[b] = saturating_increment(h->busy[b]);
    } else if (h->busy[b] > 0) {
        h->quiet[b] = saturating_increment(h->quiet[b]);
    }
    if (h->busy[b] == 0) {
        return false;
    }

    const bool settled = h->quiet[b] >= SAND_HEAL_SETTLE_FRAMES && h->since_heal[b] >= SAND_HEAL_MIN_GAP_FRAMES;
    if (!settled && h->busy[b] < SAND_HEAL_BUSY_FRAMES) {
        return false;
    }
    h->busy[b] = 0;
    h->quiet[b] = 0;
    h->since_heal[b] = 0;
    return true;
}

int
sand_heal_step(sand_heal_t* h, sand_heal_span_t* out, int max) {
    int n = 0;
    bool open = false;

    for (int b = 0; b < h->bands; b++) {
        if (!band_due(h, b)) {
            open = false;
            continue;
        }
        const int y1 = (b + 1) * SAND_HEAL_BAND_ROWS;
        if (open) {
            out[n - 1].y1 = y1;
        } else if (n < max) {
            out[n].y0 = b * SAND_HEAL_BAND_ROWS;
            out[n].y1 = y1;
            n++;
            open = true;
        }
    }
    h->moved = 0;
    if (n > 0 && out[n - 1].y1 > h->rows) {
        out[n - 1].y1 = h->rows;
    }
    return n;
}
