#include <stdlib.h>
#include "render/probe/sh_probes.h"
#include "suites.h"
#include "unity.h"

static void
test_probe_corners_mean_clamp_scale(void) {
    enum { PROBES = 8 };

    int16_t (*data)[SH_PROBES_COMPONENTS] = malloc(PROBES * sizeof *data);
    TEST_ASSERT_NOT_NULL(data);
    for (int p = 0; p < PROBES; p++) {
        for (int k = 0; k < SH_PROBES_COMPONENTS; k++) {
            data[p][k] = p * SH_PROBES_COMPONENTS + k;
        }
    }
    sh_probes_t grid = {.origin = {1, 2, 3},
                        .spacing = {2, 4, 6},
                        .dims = {2, 2, 2},
                        .scale = 0.5F,
                        .coefficients = (const int16_t (*)[SH_PROBES_COMPONENTS])data};
    for (int p = 0; p < PROBES; p++) {
        const vec3f_t position =
            vec3f_add(grid.origin, (vec3f_t){grid.spacing.x * (p & 1), grid.spacing.y * ((p >> 1) & 1),
                                             grid.spacing.z * ((p >> 2) & 1)});
        const sh1_rgb_t got = sh_probes_sample(&grid, position);
        for (int k = 0; k < SH1_COEFFICIENTS; k++) {
            TEST_ASSERT_EQUAL_FLOAT(data[p][k * SH1_CHANNELS] * grid.scale, got.c[k].x);
            TEST_ASSERT_EQUAL_FLOAT(data[p][k * SH1_CHANNELS + 1] * grid.scale, got.c[k].y);
            TEST_ASSERT_EQUAL_FLOAT(data[p][k * SH1_CHANNELS + 2] * grid.scale, got.c[k].z);
        }
    }
    const sh1_rgb_t centre = sh_probes_sample(&grid, vec3f_add(grid.origin, vec3f_scale(grid.spacing, 0.5F)));
    for (int c = 0; c < SH1_COEFFICIENTS; c++) {
        const float mean = ((PROBES - 1) * SH_PROBES_COMPONENTS * 0.5F + c * SH1_CHANNELS) * grid.scale;
        TEST_ASSERT_EQUAL_FLOAT(mean, centre.c[c].x);
        TEST_ASSERT_EQUAL_FLOAT(mean + grid.scale, centre.c[c].y);
        TEST_ASSERT_EQUAL_FLOAT(mean + 2 * grid.scale, centre.c[c].z);
    }
    const sh1_rgb_t outside = sh_probes_sample(&grid, (vec3f_t){-100, -100, -100});
    TEST_ASSERT_EQUAL_FLOAT(data[0][2] * grid.scale, outside.c[0].z);
    grid.dims[1] = grid.dims[2] = 1;
    grid.spacing.y = grid.spacing.z = 0;
    const sh1_rgb_t flat = sh_probes_sample(&grid, (vec3f_t){2, 100, -100});
    TEST_ASSERT_EQUAL_FLOAT(SH_PROBES_COMPONENTS * grid.scale * 0.5F, flat.c[0].x);
    free(data);
}

void
suite_sh_probes(void) {
    RUN_TEST(test_probe_corners_mean_clamp_scale);
}

SUITE_REGISTER(suite_sh_probes);
