#include "render/settle/render_settle.h"

#include <string.h>

#include "math/scalar/mathi.h"
#include "math/signal/smooth.h"

/* The perspective divide every pixel while refining. */
#define RENDER_SETTLE_DIVIDE_PX 1

void
render_settle_forget(render_settle_t* s) {
    memset(s->quiet, 0, sizeof s->quiet);
    memset(s->refined, 0, sizeof s->refined);
}

static int
band_row(int band, int height) {
    return band * height / RENDER_SETTLE_BANDS;
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

/* A previously refined band can connect two due bands in one raster run. */
static int
unrefined_band(const render_settle_t* s, int band, int direction) {
    while (band >= 0 && band < RENDER_SETTLE_BANDS && s->refined[band]) {
        band += direction;
    }
    return band;
}

static int
first_due_band(const render_settle_t* s, int height, int hold) {
    for (int i = 0; i < RENDER_SETTLE_BANDS; i++) {
        const int b = (RENDER_SETTLE_BANDS / 2) + (i % 2 == 0 ? i / 2 : -(i + 1) / 2);
        if (due(s, b, hold) && band_row(b, height) < band_row(b + 1, height)) {
            return b;
        }
    }
    return -1;
}

render_rows_t
render_settle_next(const render_settle_t* s, int height, render_settle_settings_t settings) {
    if (height <= 0 || settings.frame_us <= 0) {
        return (render_rows_t){0, 0};
    }
    int first = first_due_band(s, height, settings.hold_frames);
    if (first < 0) {
        return (render_rows_t){0, 0};
    }
    int end = first + 1;
    if (s->row_us == 0) {
        return (render_rows_t){band_row(first, height), band_row(end, height)};
    }
    const int64_t budget_rows = ((int64_t)settings.frame_us << RENDER_SETTLE_COST_FRAC_BITS) / s->row_us;
    bool grew;
    do {
        grew = false;
        const int before = unrefined_band(s, first - 1, -1);
        if (due(s, before, settings.hold_frames) && band_row(end, height) - band_row(before, height) <= budget_rows) {
            first = before;
            grew = true;
        }
        const int after = unrefined_band(s, end, 1);
        if (due(s, after, settings.hold_frames)
            && band_row(after + 1, height) - band_row(first, height) <= budget_rows) {
            end = after + 1;
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
    const int64_t one_us = INT64_C(1) << RENDER_SETTLE_COST_FRAC_BITS;
    const int64_t measured = (int64_t)mathi_max(cost_us, 0) * one_us / (rows.end - rows.first);
    const int64_t sample = measured < one_us ? one_us : measured;
    const int64_t learned = s->row_us == 0 ? sample : smooth_step_i64(s->row_us, sample, SMOOTH_RTT_DIVISOR);
    s->row_us = learned > INT32_MAX ? INT32_MAX : (int32_t)learned;
}

#define SETTLE_QUALITY_FIELDS(X)                                                                                       \
    X(texture.divide_px, RENDER_SETTLE_DIVIDE_PX)                                                                      \
    X(texture.affine_bend, 0.0F)                                                                                       \
    X(texture.affine_level, 0)                                                                                         \
    X(texture.far_level, 0)                                                                                            \
    X(small_fill.ceiling, SMALL_FILL_CEILING_OFF)

#define SETTLE_SET_FIELD(field, value) quality.field = (value);

render_settings_t
render_settle_quality(const render_settings_t* live) {
    render_settings_t quality = *live;
    SETTLE_QUALITY_FIELDS(SETTLE_SET_FIELD)
    return quality;
}

#define SETTLE_DIFFERS(field, value) || live->field != quality.field

bool
render_settle_refines(const render_settings_t* live) {
    const render_settings_t quality = render_settle_quality(live);
    return false SETTLE_QUALITY_FIELDS(SETTLE_DIFFERS);
}
