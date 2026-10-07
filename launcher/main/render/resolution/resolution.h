/*
 * resolution: dynamic resolution, the render size picked per frame to hold a
 * frame-cost budget. A step is one render size, finest first; the caller
 * draws at the chosen step and reports what the frame cost.
 *
 * Two policies share the steps. The stepped controller reacts: a window of
 * measured costs moves it one step at a time, with separate thresholds, a
 * cooldown that doubles after a reversal, and a panic drop for one frame far
 * over budget. The predictor acts first: its linear cost model starts from
 * the offline fit and refits from each frame's measured draw by recursive
 * least squares, with the fit as prior, bounded uncertainty, process noise
 * to keep tracking, and one outlier frame clipped. It prices every step from
 * what survived culling this frame, and the finest step that fits is drawn.
 *
 * Steps past `recovery_from` are recovery only: the stepped controller
 * reaches them by a panic drop, the predictor only when the coarsest
 * ordinary step would pass the panic share; short of that it holds that
 * step as a floor, a little over budget.
 * Pure, portable and host-tested; time is passed in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RESOLUTION_STEPS_MAX  8
#define RESOLUTION_WINDOW_MAX 16

typedef struct {
    int width, height;
} resolution_step_t;

typedef struct {
    resolution_step_t steps[RESOLUTION_STEPS_MAX];
    int step_count;
    int recovery_from; /* the first recovery-only step; step_count when none */
    int32_t budget_us;
    int window;        /* frames averaged before a step */
    int cooldown;      /* frames after a step before the next; doubles after a reversal */
    int up_percent;    /* finer when the window's mean is under this share of the budget */
    int down_percent;  /* coarser when over it */
    int panic_percent; /* one frame over this share drops `panic_steps` at once */
    int panic_steps;
} resolution_config_t;

/* `steps`, each no larger than the one before in either axis; the rest of
 * the config takes the defaults the stepped controller was tuned with on
 * the board. */
resolution_config_t resolution_config(const resolution_step_t* steps, int count, int recovery_from, int32_t budget_us);

/* A policy's state; the config is the caller's, passed to every call. */
typedef struct {
    int step;
    int32_t window_us[RESOLUTION_WINDOW_MAX];
    int filled;
    int cooldown_left;
    int cooldown;       /* the cooldown now in force */
    int last_direction; /* +1 coarser, -1 finer, 0 none yet */
    int switches;
} resolution_control_t;

void resolution_control_init(resolution_control_t* control, const resolution_config_t* config, int first_step);

/* Folds in what the last frame at the current step cost and returns the
 * step for the next one. */
int resolution_control_update(resolution_control_t* control, const resolution_config_t* config, int32_t frame_us);

/* What a step costs, as weights fitted to board frames:
 * base + triangles * (per_triangle + per_triangle_row * height share)
 *      + per_pixel_share * pixel share, plus the step's own upscale. The
 * shares are of the destination, so the weights hold at any step. */
typedef struct {
    float base_us, per_triangle_us, per_triangle_row_us, per_pixel_share_us;
    float upscale_us[RESOLUTION_STEPS_MAX];
} resolution_model_t;

/* One frame measured for a fit: its step, triangles after culling and what
 * its scaled part (draw, then upscale) cost. */
typedef struct {
    int step;
    int triangles;
    int32_t draw_us, upscale_us;
} resolution_sample_t;

/* Least squares over `count` samples for the four weights, and the mean
 * upscale of each step. False when the samples cannot tell the weights apart. */
bool resolution_model_fit(resolution_model_t* model, const resolution_config_t* config,
                          const resolution_sample_t* samples, int count);

float resolution_model_predict_us(const resolution_model_t* model, const resolution_config_t* config, int step,
                                  int triangles);

typedef struct {
    resolution_model_t model;
    int step;
    resolution_model_t prior;
    float covariance[4][4];
    float prior_variance[4];
    float chosen_us; /* model price when the step was chosen */
    int switches;
} resolution_predict_t;

void resolution_predict_init(resolution_predict_t* predict, const resolution_config_t* config,
                             const resolution_model_t* model, int first_step);

/* The step for this frame, from the triangles culling kept. */
int resolution_predict_choose(resolution_predict_t* predict, const resolution_config_t* config, int triangles);

/* Draw cost refits the weights; the offline per-step upscale stays fixed. */
void resolution_predict_measured(resolution_predict_t* predict, const resolution_config_t* config, int triangles,
                                 int32_t draw_us, int32_t upscale_us);
