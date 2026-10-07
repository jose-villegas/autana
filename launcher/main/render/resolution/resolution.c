#include "render/resolution/resolution.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* Tuned on the board: one step's cost ratio reaches 1.34, and a finer step
 * must still fit once taken, so the up threshold sits under 1 / 1.34. */
#define DEFAULT_WINDOW               8
#define DEFAULT_COOLDOWN             8
#define DEFAULT_UP_PERCENT           72
#define DEFAULT_DOWN_PERCENT         100
#define DEFAULT_PANIC_PERCENT        150
#define DEFAULT_PANIC_STEPS          2
#define COOLDOWN_MAX                 128

/* The predictor goes finer only with this much of the budget to spare, so
 * a prediction at the edge does not flip between two steps. */
#define PREDICT_FINER_MARGIN_PERCENT 8
#define CORRECTION_MIN               0.5F
#define CORRECTION_MAX               2.0F
#define CORRECTION_GAIN              0.125F

resolution_config_t
resolution_config(const resolution_step_t* steps, int count, int recovery_from, int32_t budget_us) {
    assert(count > 0 && count <= RESOLUTION_STEPS_MAX && recovery_from > 0 && recovery_from <= count);
    resolution_config_t config = {
        .step_count = count,
        .recovery_from = recovery_from,
        .budget_us = budget_us,
        .window = DEFAULT_WINDOW,
        .cooldown = DEFAULT_COOLDOWN,
        .up_percent = DEFAULT_UP_PERCENT,
        .down_percent = DEFAULT_DOWN_PERCENT,
        .panic_percent = DEFAULT_PANIC_PERCENT,
        .panic_steps = DEFAULT_PANIC_STEPS,
    };
    for (int i = 0; i < count; i++) {
        /* A caller sizes its buffers for the first step. */
        assert(i == 0 || (steps[i].width <= steps[i - 1].width && steps[i].height <= steps[i - 1].height));
        config.steps[i] = steps[i];
    }
    return config;
}

void
resolution_control_init(resolution_control_t* control, const resolution_config_t* config, int first_step) {
    assert(config->window > 0 && config->window <= RESOLUTION_WINDOW_MAX);
    assert(first_step >= 0 && first_step < config->step_count);
    *control = (resolution_control_t){.step = first_step, .cooldown = config->cooldown};
}

static bool
over_share(int32_t us, int32_t budget_us, int percent) {
    return (int64_t)us * 100 > (int64_t)budget_us * percent;
}

/* A change forgets the window, whose costs were the old step's. A change
 * back the way the last one came doubles the cooldown, so two steps that
 * straddle the budget are tried less and less often. */
static void
move_to(resolution_control_t* c, int step) {
    const int direction = step > c->step ? 1 : -1;
    if (c->last_direction != 0 && direction != c->last_direction) {
        c->cooldown = c->cooldown * 2 > COOLDOWN_MAX ? COOLDOWN_MAX : c->cooldown * 2;
    }
    c->last_direction = direction;
    c->step = step;
    c->filled = 0;
    c->cooldown_left = c->cooldown;
    c->switches++;
}

/* Pushes a frame into the window; true once it is full and the cooldown over. */
static bool
window_ready(resolution_control_t* c, const resolution_config_t* config, int32_t frame_us) {
    memmove(&c->window_us[1], &c->window_us[0], sizeof(c->window_us[0]) * (size_t)(config->window - 1));
    c->window_us[0] = frame_us;
    c->filled = c->filled < config->window ? c->filled + 1 : c->filled;
    if (c->cooldown_left > 0) {
        c->cooldown_left--;
    }
    return c->filled >= config->window && c->cooldown_left == 0;
}

static int32_t
window_mean(const resolution_control_t* c, const resolution_config_t* config) {
    int64_t sum = 0;
    for (int i = 0; i < config->window; i++) {
        sum += c->window_us[i];
    }
    return (int32_t)(sum / config->window);
}

int
resolution_control_update(resolution_control_t* c, const resolution_config_t* config, int32_t frame_us) {
    const int last = config->step_count - 1;
    if (c->step < last && over_share(frame_us, config->budget_us, config->panic_percent)) {
        const int to = c->step + config->panic_steps;
        move_to(c, to > last ? last : to);
        return c->step;
    }
    if (!window_ready(c, config, frame_us)) {
        return c->step;
    }
    const int32_t mean = window_mean(c, config);
    if (c->step < config->recovery_from - 1 && over_share(mean, config->budget_us, config->down_percent)) {
        move_to(c, c->step + 1);
    } else if (c->step >= config->recovery_from && !over_share(mean, config->budget_us, config->down_percent)) {
        move_to(c, c->step - 1); /* recovery is left as soon as it is under budget */
    } else if (c->step > 0 && !over_share(mean, config->budget_us, config->up_percent)) {
        move_to(c, c->step - 1);
    } else if (c->cooldown > config->cooldown) {
        c->cooldown--; /* each settled frame earns one back */
    }
    return c->step;
}

static void
features(const resolution_config_t* config, int step, int triangles, float out[4]) {
    const resolution_step_t* full = &config->steps[0];
    const resolution_step_t* s = &config->steps[step];
    const float height_share = (float)s->height / (float)full->height;
    const float pixel_share = (float)(s->width * s->height) / (float)(full->width * full->height);
    out[0] = 1.0F;
    out[1] = (float)triangles;
    out[2] = (float)triangles * height_share;
    out[3] = pixel_share;
}

/* Swaps row `col` with the row below it holding the largest entry in `col`;
 * false when every one is zero, as for weights the samples cannot separate. */
static bool
pivot_rows(float a[4][5], int col) {
    int pivot = col;
    for (int row = col + 1; row < 4; row++) {
        pivot = fabsf(a[row][col]) > fabsf(a[pivot][col]) ? row : pivot;
    }
    if (fabsf(a[pivot][col]) < 1e-9F) {
        return false;
    }
    for (int k = 0; k < 5; k++) {
        const float t = a[col][k];
        a[col][k] = a[pivot][k];
        a[pivot][k] = t;
    }
    return true;
}

/* Gauss-Jordan elimination with partial pivoting on a 4x4 system. */
static bool
solve4(float a[4][5]) {
    for (int col = 0; col < 4; col++) {
        if (!pivot_rows(a, col)) {
            return false;
        }
        for (int row = 0; row < 4; row++) {
            const float f = row == col ? 0.0F : a[row][col] / a[col][col];
            for (int k = col; k < 5; k++) {
                a[row][k] -= f * a[col][k];
            }
        }
    }
    for (int row = 0; row < 4; row++) {
        a[row][4] /= a[row][row];
    }
    return true;
}

bool
resolution_model_fit(resolution_model_t* model, const resolution_config_t* config, const resolution_sample_t* samples,
                     int count) {
    /* Triangles are thousands, shares are one: scaled to one size so the
     * normal equations stay well conditioned. */
    const float scale[4] = {1.0F, 1e-3F, 1e-3F, 1.0F};
    float a[4][5] = {{0}};
    float upscale_sum[RESOLUTION_STEPS_MAX] = {0};
    int upscale_n[RESOLUTION_STEPS_MAX] = {0};
    for (int i = 0; i < count; i++) {
        const resolution_sample_t* s = &samples[i];
        float x[4];
        features(config, s->step, s->triangles, x);
        for (int r = 0; r < 4; r++) {
            for (int k = 0; k < 4; k++) {
                a[r][k] += x[r] * scale[r] * x[k] * scale[k];
            }
            a[r][4] += x[r] * scale[r] * (float)s->draw_us;
        }
        upscale_sum[s->step] += (float)s->upscale_us;
        upscale_n[s->step]++;
    }
    if (!solve4(a)) {
        return false;
    }
    *model = (resolution_model_t){
        .base_us = a[0][4] * scale[0],
        .per_triangle_us = a[1][4] * scale[1],
        .per_triangle_row_us = a[2][4] * scale[2],
        .per_pixel_share_us = a[3][4] * scale[3],
    };
    for (int step = 0; step < config->step_count; step++) {
        model->upscale_us[step] = upscale_n[step] > 0 ? upscale_sum[step] / (float)upscale_n[step] : 0.0F;
    }
    return true;
}

float
resolution_model_predict_us(const resolution_model_t* model, const resolution_config_t* config, int step,
                            int triangles) {
    float x[4];
    features(config, step, triangles, x);
    return model->base_us + (model->per_triangle_us * x[1]) + (model->per_triangle_row_us * x[2])
           + (model->per_pixel_share_us * x[3]) + model->upscale_us[step];
}

void
resolution_predict_init(resolution_predict_t* predict, const resolution_config_t* config,
                        const resolution_model_t* model, int first_step) {
    assert(first_step >= 0 && first_step < config->step_count);
    *predict = (resolution_predict_t){.model = *model, .step = first_step, .correction = 1.0F};
}

static float
corrected_us(const resolution_predict_t* p, const resolution_config_t* config, int step, int triangles) {
    return p->correction * resolution_model_predict_us(&p->model, config, step, triangles);
}

/* The finest step whose corrected cost fits `share` of the budget, among
 * the first `count`; -1 when none does. */
static int
finest_fitting(const resolution_predict_t* p, const resolution_config_t* config, int triangles, int count,
               int percent) {
    const float limit = (float)config->budget_us * (float)percent / 100.0F;
    for (int step = 0; step < count; step++) {
        if (corrected_us(p, config, step, triangles) <= limit) {
            return step;
        }
    }
    return -1;
}

int
resolution_predict_choose(resolution_predict_t* p, const resolution_config_t* config, int triangles) {
    int chosen = finest_fitting(p, config, triangles, config->recovery_from, config->down_percent);
    if (chosen < 0) {
        /* Over budget at every ordinary step: the coarsest one is the floor,
         * as a little over budget costs less than a harsh drop in picture,
         * unless even it would pass the panic share. */
        const int floor = config->recovery_from - 1;
        const float panic = (float)config->budget_us * (float)config->panic_percent / 100.0F;
        chosen = corrected_us(p, config, floor, triangles) <= panic ? floor : -1;
    }
    if (chosen < 0) {
        chosen = finest_fitting(p, config, triangles, config->step_count, config->panic_percent);
    }
    if (chosen < 0) {
        chosen = config->step_count - 1;
    }
    if (chosen < p->step) {
        const int margin = config->down_percent - PREDICT_FINER_MARGIN_PERCENT;
        const int finer = finest_fitting(p, config, triangles, p->step, margin);
        chosen = finer < 0 ? p->step : finer;
    }
    if (chosen != p->step) {
        p->step = chosen;
        p->switches++;
    }
    p->chosen_us = corrected_us(p, config, p->step, triangles);
    return p->step;
}

void
resolution_predict_measured(resolution_predict_t* p, const resolution_config_t* config, int triangles,
                            int32_t frame_us) {
    const float predicted = resolution_model_predict_us(&p->model, config, p->step, triangles);
    if (predicted <= 0.0F) {
        return;
    }
    const float ratio = (float)frame_us / predicted;
    float c = p->correction + (CORRECTION_GAIN * (ratio - p->correction));
    c = c < CORRECTION_MIN ? CORRECTION_MIN : c;
    p->correction = c > CORRECTION_MAX ? CORRECTION_MAX : c;
}
