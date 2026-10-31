/* The dynamic-resolution replay's probe: the engine's own fit and predictor
 * over board frames, so dynres_replay.py holds no copy of either.
 *
 *   probe fit < samples      the model fitted to every sample, then each sample's price
 *   probe replay budget_us margin_percent up_hold_us top first < frames
 *                            the step the predictor draws each frame at, its corrected
 *                            price there and at the next finer step (0 at the top),
 *                            and its switches
 *
 * Both inputs start with the ladder: "steps n recovery" and n "width height".
 * samples then holds "m" and m "step triangles draw_us upscale_us". frames
 * holds the model's four weights and one upscale per step, then "f", then f
 * rows of n "whole_us triangles draw_us upscale_us", one per step. The
 * time between two choices is the whole frame drawn between them. Output is JSON. */
#include <stdio.h>
#include <stdlib.h>

#include "render/resolution/resolution.h"

static int
read_int(void) {
    int value;
    if (scanf("%d", &value) != 1) {
        fprintf(stderr, "dynres_replay_probe: input ended early\n");
        exit(1);
    }
    return value;
}

static resolution_config_t
read_ladder(int32_t budget_us) {
    resolution_step_t steps[RESOLUTION_STEPS_MAX];
    char word[8];
    if (scanf("%7s", word) != 1) {
        exit(1);
    }
    const int count = read_int();
    const int recovery_from = read_int();
    for (int i = 0; i < count; i++) {
        steps[i].width = read_int();
        steps[i].height = read_int();
    }
    return resolution_config(steps, count, recovery_from, budget_us);
}

static void
print_model(const resolution_model_t* m, const resolution_config_t* config) {
    printf("\"model\": [%.4f, %.6f, %.6f, %.4f], \"upscale\": [", (double)m->base_us, (double)m->per_triangle_us,
           (double)m->per_triangle_row_us, (double)m->per_pixel_share_us);
    for (int i = 0; i < config->step_count; i++) {
        printf("%s%.1f", i == 0 ? "" : ", ", (double)m->upscale_us[i]);
    }
    printf("]");
}

static int
fit(void) {
    const resolution_config_t config = read_ladder(1);
    const int count = read_int();
    resolution_sample_t* samples = malloc(sizeof(*samples) * (size_t)count);
    if (samples == NULL) {
        return 1;
    }
    for (int i = 0; i < count; i++) {
        samples[i].step = read_int();
        samples[i].triangles = read_int();
        samples[i].draw_us = read_int();
        samples[i].upscale_us = read_int();
    }
    resolution_model_t model;
    if (!resolution_model_fit(&model, &config, samples, count)) {
        fprintf(stderr, "dynres_replay_probe: the samples cannot tell the weights apart\n");
        return 1;
    }
    printf("{");
    print_model(&model, &config);
    printf(", \"predicted\": [");
    for (int i = 0; i < count; i++) {
        printf("%s%.0f", i == 0 ? "" : ", ",
               (double)resolution_model_predict_us(&model, &config, samples[i].step, samples[i].triangles));
    }
    printf("]}\n");
    free(samples);
    return 0;
}

static int
replay(char** argv) {
    resolution_config_t config = read_ladder(atoi(argv[0]));
    config.up_margin_percent = atoi(argv[1]);
    config.up_hold_us = atoi(argv[2]);
    config.top = atoi(argv[3]);
    const int first = atoi(argv[4]);
    float weights[4];
    for (int i = 0; i < 4; i++) {
        if (scanf("%f", &weights[i]) != 1) {
            return 1;
        }
    }
    resolution_model_t model = {weights[0], weights[1], weights[2], weights[3], {0}};
    for (int i = 0; i < config.step_count; i++) {
        if (scanf("%f", &model.upscale_us[i]) != 1) {
            return 1;
        }
    }
    const int frames = read_int();
    resolution_predict_t predict;
    resolution_predict_init(&predict, &config, &model, first);
    int32_t* priced_us = malloc(sizeof(*priced_us) * 2 * (size_t)(frames > 0 ? frames : 1));
    if (priced_us == NULL) {
        return 1;
    }
    int32_t* finer_us = priced_us + frames;
    printf("{\"steps\": [");
    int32_t elapsed_us = 0;
    for (int f = 0; f < frames; f++) {
        int32_t whole_us[RESOLUTION_STEPS_MAX];
        int triangles[RESOLUTION_STEPS_MAX];
        int32_t cost_us[RESOLUTION_STEPS_MAX];
        for (int s = 0; s < config.step_count; s++) {
            whole_us[s] = read_int();
            triangles[s] = read_int();
            cost_us[s] = read_int();
            cost_us[s] += read_int();
        }
        /* The census before the choice counts what culling keeps, the same
         * at every step: the step drawn last frame supplies it. */
        const int step = resolution_predict_choose(&predict, &config, triangles[predict.step], elapsed_us);
        priced_us[f] = (int32_t)predict.chosen_us;
        /* The price the up margin guards: the next finer step's, corrected. */
        finer_us[f] =
            step == config.top
                ? 0
                : (int32_t)((predict.scale
                             * resolution_model_predict_us(&predict.model, &config, step - 1, triangles[step]))
                            + predict.offset_us);
        resolution_predict_measured(&predict, &config, triangles[step], cost_us[step]);
        elapsed_us = whole_us[step];
        printf("%s%d", f == 0 ? "" : ", ", step);
    }
    printf("], \"priced\": [");
    for (int f = 0; f < frames; f++) {
        printf("%s%ld", f == 0 ? "" : ", ", (long)priced_us[f]);
    }
    printf("], \"finer\": [");
    for (int f = 0; f < frames; f++) {
        printf("%s%ld", f == 0 ? "" : ", ", (long)finer_us[f]);
    }
    printf("], \"switches\": %d}\n", predict.switches);
    free(priced_us);
    return 0;
}

int
main(int argc, char** argv) {
    if (argc == 2 && argv[1][0] == 'f') {
        return fit();
    }
    if (argc == 7 && argv[1][0] == 'r') {
        return replay(argv + 2);
    }
    fprintf(stderr, "usage: dynres_replay_probe fit | replay budget_us margin_percent up_hold_us top first\n");
    return 2;
}
