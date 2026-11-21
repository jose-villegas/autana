#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "render/probe/sh_probes.h"
#include "scene/scene_asset.h"
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

static void
test_python_probe_grid(void) {
    enum {
        GOLDEN_PROBES = 2,
        GOLDEN_FIRST = -12,
        GOLDEN_BYTES = SH_PROBES_HEADER_SIZE + GOLDEN_PROBES * SH_PROBES_RECORD_SIZE + sizeof(uint8_t)
    };

    const float golden_scale = 0.25F;
    static const uint8_t golden[] = "\x01\x00\x01\x00\x00\x00\x80\x3f\x00\x00\x00\x40\x00\x00\x40\x40"
                                    "\x00\x00\x80\x40\x00\x00\xa0\x40\x00\x00\xc0\x40\x00\x00\x80\x3e"
                                    "\x02\x00\x01\x00\x01\x00\x00\x00\xf4\xff\xf5\xff\xf6\xff\xf7\xff"
                                    "\xf8\xff\xf9\xff\xfa\xff\xfb\xff\xfc\xff\xfd\xff\xfe\xff\xff\xff"
                                    "\x00\x00\x01\x00\x02\x00\x03\x00\x04\x00\x05\x00\x06\x00\x07\x00"
                                    "\x08\x00\x09\x00\x0a\x00\x0b\x00\x01";
    _Static_assert(sizeof golden == GOLDEN_BYTES + sizeof "", "golden string terminator");
    _Alignas(int16_t) uint8_t bytes[GOLDEN_BYTES];
    memcpy(bytes, golden, sizeof bytes);
#ifndef DEVICE_BUILD
    const char* path = getenv("AUTANA_SH_PROBES");
    TEST_ASSERT_NOT_NULL(path);
    FILE* file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_UINT(sizeof bytes, fread(bytes, 1, sizeof bytes, file));
    TEST_ASSERT_EQUAL_INT(EOF, fgetc(file));
    fclose(file);
    TEST_ASSERT_EQUAL_MEMORY(golden, bytes, sizeof bytes);
#endif
    const asset_view_t entry = {.data = (const uint8_t*)bytes, .size = sizeof bytes};
    sh_probes_t grid;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, sh_probes_read(entry, &grid));
    TEST_ASSERT_EQUAL_PTR(entry.data + SH_PROBES_HEADER_SIZE, grid.coefficients);
    TEST_ASSERT_EQUAL_PTR(entry.data + GOLDEN_BYTES - sizeof(uint8_t), grid.inside);
    TEST_ASSERT_TRUE(sh_probes_inside(&grid, 0, 0, 0));
    TEST_ASSERT_FALSE(sh_probes_inside(&grid, 1, 0, 0));
    TEST_ASSERT_EQUAL_FLOAT(1, grid.origin.x);
    TEST_ASSERT_EQUAL_FLOAT(2, grid.origin.y);
    TEST_ASSERT_EQUAL_FLOAT(3, grid.origin.z);
    TEST_ASSERT_EQUAL_FLOAT(4, grid.spacing.x);
    TEST_ASSERT_EQUAL_FLOAT(5, grid.spacing.y);
    TEST_ASSERT_EQUAL_FLOAT(6, grid.spacing.z);
    TEST_ASSERT_EQUAL_UINT16(GOLDEN_PROBES, grid.dims[0]);
    TEST_ASSERT_EQUAL_UINT16(1, grid.dims[1]);
    TEST_ASSERT_EQUAL_UINT16(1, grid.dims[2]);
    TEST_ASSERT_EQUAL_FLOAT(golden_scale, grid.scale);
    for (int p = 0; p < GOLDEN_PROBES; p++) {
        const vec3f_t position = vec3f_add(grid.origin, (vec3f_t){p * grid.spacing.x, 0, 0});
        const sh1_rgb_t got = sh_probes_sample(&grid, position);
        for (int c = 0; c < SH1_COEFFICIENTS; c++) {
            const float channels[] = {got.c[c].x, got.c[c].y, got.c[c].z};
            for (int rgb = 0; rgb < SH1_CHANNELS; rgb++) {
                const int component = c * SH1_CHANNELS + rgb;
                const int expected = GOLDEN_FIRST + p * SH_PROBES_COMPONENTS + component;
                TEST_ASSERT_EQUAL_INT16(expected, grid.coefficients[p][component]);
                TEST_ASSERT_EQUAL_FLOAT(expected * golden_scale, channels[rgb]);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_SIZE, sh_probes_read((asset_view_t){entry.data, entry.size - 1}, &grid));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_SIZE, sh_probes_read((asset_view_t){entry.data, entry.size + 1}, &grid));
    bytes[GOLDEN_BYTES - sizeof(uint8_t)] |= 1U << GOLDEN_PROBES;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, sh_probes_read(entry, &grid));
    memcpy(bytes, golden, sizeof bytes);
    bytes[SH_PROBES_AT_FLAGS] = SH_PROBES_FLAG_INSIDE_MASK << 1;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, sh_probes_read(entry, &grid));
    bytes[SH_PROBES_AT_FLAGS] = 0;
    const asset_view_t unmasked = {entry.data, entry.size - sizeof(uint8_t)};
    TEST_ASSERT_EQUAL_INT(ASSET_OK, sh_probes_read(unmasked, &grid));
    TEST_ASSERT_NULL(grid.inside);
    for (int x = 0; x < GOLDEN_PROBES; x++) {
        TEST_ASSERT_FALSE(sh_probes_inside(&grid, x, 0, 0));
    }
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_SIZE, sh_probes_read(entry, &grid));
    memcpy(bytes, golden, sizeof bytes);
    ((uint8_t*)bytes)[SH_PROBES_AT_VERSION] = 0;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, sh_probes_read(entry, &grid));
    ((uint8_t*)bytes)[SH_PROBES_AT_VERSION] = SH_PROBES_VERSION;
    memset((uint8_t*)bytes + SH_PROBES_AT_DIMS, 0, sizeof(uint16_t));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, sh_probes_read(entry, &grid));
}

static void
test_probe_reader_bounds(void) {
    uint16_t bytes[(SH_PROBES_HEADER_SIZE + SH_PROBES_RECORD_SIZE) / sizeof(uint16_t)] = {0};
    uint8_t* data = (uint8_t*)bytes;
    data[SH_PROBES_AT_VERSION] = SH_PROBES_VERSION;
    const uint16_t dims[MAT3F_AXES] = {1, 1, 1};
    const vec3f_t spacing = vec3f_one();
    const float scale = 1;
    memcpy(data + SH_PROBES_AT_DIMS, dims, sizeof dims);
    memcpy(data + SH_PROBES_AT_SPACING, &spacing, sizeof spacing);
    memcpy(data + SH_PROBES_AT_SCALE, &scale, sizeof scale);
    const asset_view_t entry = {data, sizeof bytes};
    sh_probes_t grid;
    TEST_ASSERT_EQUAL_INT(ASSET_OK, sh_probes_read(entry, &grid));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, sh_probes_read((asset_view_t){NULL, entry.size}, &grid));
    TEST_ASSERT_NULL(grid.coefficients);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, sh_probes_read((asset_view_t){data, SH_PROBES_HEADER_SIZE - 1}, &grid));
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_BOUNDS, sh_probes_read((asset_view_t){data + 1, entry.size - 1}, &grid));
    data[SH_PROBES_AT_VERSION + 1] = 1;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_VERSION, sh_probes_read(entry, &grid));
    data[SH_PROBES_AT_VERSION + 1] = 0;
    data[SH_PROBES_AT_FLAGS] = SH_PROBES_FLAG_INSIDE_MASK << 1;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, sh_probes_read(entry, &grid));
    data[SH_PROBES_AT_FLAGS] = 0;
    data[SH_PROBES_AT_PADDING] = 1;
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, sh_probes_read(entry, &grid));
    data[SH_PROBES_AT_PADDING] = 0;
    const uint16_t huge_dims[MAT3F_AXES] = {UINT16_MAX, UINT16_MAX, UINT16_MAX};
    memcpy(data + SH_PROBES_AT_DIMS, huge_dims, sizeof huge_dims);
    TEST_ASSERT_EQUAL_INT(ASSET_ERR_SIZE, sh_probes_read(entry, &grid));
    memcpy(data + SH_PROBES_AT_DIMS, dims, sizeof dims);
    const float invalid[] = {0, -1, INFINITY, NAN};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        memcpy(data + SH_PROBES_AT_SCALE, &invalid[i], sizeof invalid[i]);
        TEST_ASSERT_EQUAL_INT(ASSET_ERR_FORMAT, sh_probes_read(entry, &grid));
        TEST_ASSERT_NULL(grid.coefficients);
    }
}

void
suite_sh_probes(void) {
    RUN_TEST(test_python_probe_grid);
    RUN_TEST(test_probe_reader_bounds);
    RUN_TEST(test_probe_corners_mean_clamp_scale);
}

SUITE_REGISTER(suite_sh_probes);
