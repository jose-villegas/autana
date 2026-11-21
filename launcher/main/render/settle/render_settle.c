#include "render/settle/render_settle.h"

#include <string.h>

#include "math/scalar/mathi.h"

void
render_settle_forget(render_settle_t* s) {
    memset(s, 0, sizeof(*s));
}

static int
band_row(int band, int height) {
    return (int)((int64_t)band * height / RENDER_SETTLE_BANDS);
}

void
render_settle_age(render_settle_t* s, int height, render_rows_t drawn) {
    for (int b = 0; b < RENDER_SETTLE_BANDS; b++) {
        const int first = band_row(b, height);
        const int end = band_row(b + 1, height);
        if (drawn.first < drawn.end && first < drawn.end && end > drawn.first) {
            s->quiet[b] = 0;
            s->refined[b] = false;
        } else if (s->quiet[b] < RENDER_SETTLE_HOLD_MAX) {
            s->quiet[b]++;
        }
    }
}

static bool
due(const render_settle_t* s, int band, int hold) {
    return band >= 0 && band < RENDER_SETTLE_BANDS && !s->refined[band] && s->quiet[band] >= hold;
}

render_rows_t
render_settle_next(const render_settle_t* s, int height, render_settle_settings_t settings) {
    if (height <= 0 || settings.frame_us <= 0) {
        return (render_rows_t){0, 0};
    }
    int first = -1;
    for (int i = 0; i < RENDER_SETTLE_BANDS; i++) {
        const int b = RENDER_SETTLE_BANDS / 2 + (i % 2 == 0 ? i / 2 : -(i + 1) / 2);
        if (due(s, b, settings.hold_frames) && band_row(b, height) < band_row(b + 1, height)) {
            first = b;
            break;
        }
    }
    if (first < 0) {
        return (render_rows_t){0, 0};
    }
    int end = first + 1;
    const int32_t row_us = mathi_max(s->row_us, 1);
    bool grew;
    do {
        grew = false;
        if (due(s, first - 1, settings.hold_frames)
            && (int64_t)(band_row(end, height) - band_row(first - 1, height)) * row_us <= settings.frame_us) {
            first--;
            grew = true;
        }
        if (due(s, end, settings.hold_frames)
            && (int64_t)(band_row(end + 1, height) - band_row(first, height)) * row_us <= settings.frame_us) {
            end++;
            grew = true;
        }
    } while (grew);
    return (render_rows_t){band_row(first, height), band_row(end, height)};
}

void
render_settle_done(render_settle_t* s, int height, render_rows_t rows, int32_t cost_us) {
    if (rows.first >= rows.end) {
        return;
    }
    for (int b = 0; b < RENDER_SETTLE_BANDS; b++) {
        if (band_row(b, height) >= rows.first && band_row(b + 1, height) <= rows.end) {
            s->refined[b] = true;
        }
    }
    const int32_t sample = mathi_max(cost_us / (rows.end - rows.first), 1);
    s->row_us = s->row_us == 0 ? sample : s->row_us + (sample - s->row_us) / RENDER_SETTLE_COST_WEIGHT_DIVISOR;
}

render_settings_t
render_settle_quality(const render_settings_t* live) {
    render_settings_t quality = *live;
    quality.texture = (render_texture_settings_t){.divide_px = RENDER_SETTLE_DIVIDE_PX};
    quality.small_fill.ceiling = SMALL_FILL_CEILING_OFF;
    return quality;
}

bool
render_settle_refines(const render_settings_t* live) {
    return live->texture.divide_px != RENDER_SETTLE_DIVIDE_PX || live->texture.affine_bend != 0.0F
           || live->texture.affine_level != 0 || live->texture.far_level != 0
           || live->small_fill.ceiling != SMALL_FILL_CEILING_OFF;
}
