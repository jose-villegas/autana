/*
 * Portable suite: flip3.h, water in a box. What a person holding the tank
 * would check: the fill is the volume asked for, the water settles level
 * across gravity, lies on whichever glass is down, keeps its volume, and
 * the same inputs give the same water.
 */

#include <stdlib.h>
#include <string.h>

#include "testkit/cleanup.h"
#include "testkit/suite.h"
#include "unity.h"

#include "physics/fluid/flip3.h"

/* This suite's own tank, 6 x 8 x 3 cells: small enough that its buffers
 * fit the board's internal RAM and a few simulated seconds stay quick.
 * Units are arbitrary, gravity included. */
#define BOX_X         120.0F
#define BOX_Y         160.0F
#define BOX_Z         60.0F
#define CELL          20.0F
#define MAX_PARTICLES 800
#define GRAVITY       3000.0F
#define DT            (1.0F / 60.0F)
#define SETTLE_STEPS  300  /* five simulated seconds */
#define LONG_STEPS    1800 /* thirty */
#define SAMPLES       2    /* density field samples per cell */

static flip3_t water;
static void* grid;
static void* particles;
static float* field;
static int fx, fy, fz;

static const flip3_config_t CONFIG = {{BOX_X, BOX_Y, BOX_Z}, CELL, MAX_PARTICLES};

/* A box as the device's is: no axis a whole number of cells, so each axis
 * rounds its own spacing (6.85, 8.65 and 3.05 cells here). */
static const flip3_config_t ODD = {{137.0F, 173.0F, 61.0F}, CELL, MAX_PARTICLES};

static void
fixture_end(void) {
    free(grid);
    free(particles);
    free(field);
    grid = particles = NULL;
    field = NULL;
}

/* Freed after the test even when an assertion fails. */
static void
fixture_with(const flip3_config_t* config) {
    suite_add_test_cleanup(fixture_end);
    grid = malloc(flip3_grid_bytes(config));
    particles = malloc(flip3_particle_bytes(config));
    TEST_ASSERT_TRUE(flip3_init(&water, config, grid, particles));
    field = malloc(sizeof(float) * flip3_field_count(&water, SAMPLES));
    TEST_ASSERT_NOT_NULL(field);
}

static void
fixture_begin(void) {
    fixture_with(&CONFIG);
}

/* The surface field, its sizes kept for sample(). */
static void
take_field(void) {
    const iso_field_t f = flip3_field(&water, SAMPLES, field);
    fx = f.nx;
    fy = f.ny;
    fz = f.nz;
}

static void
run(int steps, vec3f_t accel) {
    for (int i = 0; i < steps; i++) {
        flip3_step(&water, DT, accel);
    }
}

static float
sample(int x, int y, int z) {
    return field[(((x * fy) + y) * fz) + z];
}

/* The share of the box the density field calls water. */
static float
wet_share(void) {
    take_field();
    int wet = 0;
    for (int x = 1; x < fx - 1; x++) {
        for (int y = 1; y < fy - 1; y++) {
            for (int z = 1; z < fz - 1; z++) {
                wet += sample(x, y, z) > 0.5F ? 1 : 0;
            }
        }
    }
    return (float)wet / (float)((fx - 2) * (fy - 2) * (fz - 2));
}

static float
rms_speed(void) {
    float sum = 0.0F;
    for (int i = 0; i < water.count; i++) {
        sum += vec3f_dot(water.vel[i], water.vel[i]);
    }
    return sqrtf(sum / (float)water.count);
}

static void
test_a_bad_configuration_is_refused(void) {
    const flip3_config_t no_size = {{0.0F, BOX_Y, BOX_Z}, CELL, MAX_PARTICLES};
    const flip3_config_t no_cell = {{BOX_X, BOX_Y, BOX_Z}, 0.0F, MAX_PARTICLES};
    const flip3_config_t none = {{BOX_X, BOX_Y, BOX_Z}, CELL, 0};
    uint8_t block[16];
    flip3_t f;
    TEST_ASSERT_EQUAL_size_t(0, flip3_grid_bytes(&no_size));
    TEST_ASSERT_EQUAL_size_t(0, flip3_particle_bytes(&no_cell));
    TEST_ASSERT_FALSE(flip3_init(&f, &no_size, block, block));
    TEST_ASSERT_FALSE(flip3_init(&f, &no_cell, block, block));
    TEST_ASSERT_FALSE(flip3_init(&f, &none, block, block));
    TEST_ASSERT_FALSE(flip3_init(&f, &CONFIG, NULL, block));
    TEST_ASSERT_FALSE(flip3_init(&f, &CONFIG, block, NULL));
}

static void
test_whole_cells_fill_the_box_on_every_axis(void) {
    fixture_begin();
    TEST_ASSERT_EQUAL_INT(6 + 2, water.nx);
    TEST_ASSERT_EQUAL_INT(8 + 2, water.ny);
    TEST_ASSERT_EQUAL_INT(3 + 2, water.nz);
    fixture_end();
    fixture_with(&ODD);
    TEST_ASSERT_EQUAL_FLOAT(ODD.size.x, water.h.x * (float)(water.nx - 2));
    TEST_ASSERT_EQUAL_FLOAT(ODD.size.y, water.h.y * (float)(water.ny - 2));
    TEST_ASSERT_EQUAL_FLOAT(ODD.size.z, water.h.z * (float)(water.nz - 2));
    /* The field spans the box: its first and last samples sit half a sample in. */
    const iso_field_t f = flip3_field(&water, SAMPLES, field);
    TEST_ASSERT_FLOAT_WITHIN(1e-3F, 0.5F * f.step.x, f.origin.x + f.step.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3F, ODD.size.y - (0.5F * f.step.y), f.origin.y + ((float)(f.ny - 2) * f.step.y));
}

/* 1.5 samples a cell: each axis rounds its own count, 6, 8 and 3 cells
 * giving 9, 12 and 5 (4.5 rounded up), and still spans the box. */
static void
test_a_fractional_sample_rate_rounds_each_axis_and_spans_the_box(void) {
    fixture_begin();
    const float rate = 1.5F;
    const iso_field_t f = flip3_field(&water, rate, field);
    TEST_ASSERT_EQUAL_INT(9 + 2, f.nx);
    TEST_ASSERT_EQUAL_INT(12 + 2, f.ny);
    TEST_ASSERT_EQUAL_INT(5 + 2, f.nz);
    TEST_ASSERT_EQUAL_size_t((size_t)f.nx * (size_t)f.ny * (size_t)f.nz, flip3_field_count(&water, rate));
    TEST_ASSERT_FLOAT_WITHIN(1e-3F, BOX_X, f.step.x * (float)(f.nx - 2));
    TEST_ASSERT_FLOAT_WITHIN(1e-3F, BOX_Y, f.step.y * (float)(f.ny - 2));
    TEST_ASSERT_FLOAT_WITHIN(1e-3F, BOX_Z, f.step.z * (float)(f.nz - 2));
    TEST_ASSERT_FLOAT_WITHIN(1e-3F, 0.5F * f.step.z, f.origin.z + f.step.z);
}

/* Settled water stands at the same level whatever the rate, within a
 * sample of the coarser field. */
static void
test_a_fractional_rate_finds_the_same_level(void) {
    fixture_begin();
    TEST_ASSERT_GREATER_THAN_INT(0, flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F}));
    run(SETTLE_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    const float fine = wet_share();
    const iso_field_t coarse = flip3_field(&water, 1.5F, field);
    int wet = 0;
    for (int x = 1; x < coarse.nx - 1; x++) {
        for (int y = 1; y < coarse.ny - 1; y++) {
            for (int z = 1; z < coarse.nz - 1; z++) {
                wet += field[(((x * coarse.ny) + y) * coarse.nz) + z] > 0.5F ? 1 : 0;
            }
        }
    }
    const float share = (float)wet / (float)((coarse.nx - 2) * (coarse.ny - 2) * (coarse.nz - 2));
    TEST_ASSERT_FLOAT_WITHIN(1.0F / (float)(coarse.ny - 2), fine, share);
}

static void
test_a_fill_places_the_volume_asked_for(void) {
    fixture_begin();
    for (int percent = 10; percent <= 90; percent += 10) {
        const float wanted = (float)percent / 100.0F;
        TEST_ASSERT_GREATER_THAN_INT(0, flip3_fill(&water, wanted, (vec3f_t){0.0F, 1.0F, 0.0F}));
        TEST_ASSERT_FLOAT_WITHIN(1.0F / (float)water.seed_count, wanted, flip3_fraction(&water));
    }
    TEST_ASSERT_EQUAL_INT(0, flip3_fill(&water, 0.0F, (vec3f_t){0.0F, 1.0F, 0.0F}));
}

static void
test_a_fill_starts_at_the_bottom_along_down(void) {
    fixture_begin();
    const float fraction = 0.3F;
    flip3_fill(&water, fraction, (vec3f_t){0.0F, 1.0F, 0.0F});
    const float level = BOX_Y * (1.0F - fraction);
    for (int i = 0; i < water.count; i++) {
        TEST_ASSERT_TRUE(flip3_particle(&water, i).y >= level - water.seed_step.y);
    }
    flip3_fill(&water, fraction, (vec3f_t){-1.0F, 0.0F, 0.0F});
    for (int i = 0; i < water.count; i++) {
        TEST_ASSERT_TRUE(flip3_particle(&water, i).x <= (BOX_X * fraction) + water.seed_step.x);
    }
}

static void
test_a_fill_is_capped_by_the_particles_there_is_room_for(void) {
    const flip3_config_t small = {{BOX_X, BOX_Y, BOX_Z}, CELL, 100};
    fixture_begin();
    free(grid);
    free(particles);
    grid = malloc(flip3_grid_bytes(&small));
    particles = malloc(flip3_particle_bytes(&small));
    TEST_ASSERT_TRUE(flip3_init(&water, &small, grid, particles));
    TEST_ASSERT_EQUAL_INT(100, flip3_fill(&water, 1.0F, (vec3f_t){0.0F, 1.0F, 0.0F}));
}

/* Upright: the water comes to rest with a level surface, every column of
 * the screen's width wet to the same height within a cell. */
static void
test_upright_water_settles_level(void) {
    fixture_begin();
    flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(SETTLE_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    TEST_ASSERT_LESS_THAN_FLOAT(CELL, rms_speed()); /* under a cell a second: at rest */
    take_field();
    float lowest_top = 0.0F;
    float highest_top = BOX_Y;
    for (int x = 1; x < fx - 1; x++) {
        int top = fy - 1;
        for (int y = 1; y < fy - 1 && top == fy - 1; y++) {
            if (sample(x, y, fz / 2) > 0.5F) {
                top = y;
            }
        }
        const float at = (float)top * BOX_Y / (float)(fy - 2);
        lowest_top = fmaxf(lowest_top, at);
        highest_top = fminf(highest_top, at);
    }
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(CELL, lowest_top - highest_top);
    TEST_ASSERT_FLOAT_WITHIN(CELL, BOX_Y * 0.5F, highest_top);
}

/* Flat on its back: a layer over the whole back glass, none at the front. */
static void
test_flat_water_lies_as_a_layer_on_the_glass_below(void) {
    fixture_begin();
    flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(SETTLE_STEPS, (vec3f_t){0.0F, 0.0F, -GRAVITY});
    take_field();
    for (int x = 1; x < fx - 1; x++) {
        for (int y = 1; y < fy - 1; y++) {
            TEST_ASSERT_TRUE(sample(x, y, 1) > 0.5F);
            TEST_ASSERT_TRUE(sample(x, y, fz - 2) < 0.5F);
        }
    }
}

/* Tipped toward the viewer, the same water gathers on the front glass. */
static void
test_tipped_toward_the_viewer_water_gathers_at_the_front(void) {
    fixture_begin();
    flip3_fill(&water, 0.3F, (vec3f_t){0.0F, 0.0F, -1.0F});
    run(SETTLE_STEPS, (vec3f_t){0.0F, 0.0F, GRAVITY});
    for (int i = 0; i < water.count; i++) {
        TEST_ASSERT_TRUE(flip3_particle(&water, i).z > BOX_Z * 0.5F);
    }
}

/* Particles never leave the box, however it is thrown. */
static void
test_a_shaken_tank_keeps_every_particle_inside(void) {
    fixture_begin();
    flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F});
    const vec3f_t shakes[] = {
        {8.0F * GRAVITY, 0.0F, 0.0F}, {-8.0F * GRAVITY, GRAVITY, 0.0F}, {0.0F, -8.0F * GRAVITY, 4.0F * GRAVITY}};
    for (int round = 0; round < 6; round++) {
        run(10, shakes[round % 3]);
    }
    for (int i = 0; i < water.count; i++) {
        const vec3f_t p = flip3_particle(&water, i);
        TEST_ASSERT_TRUE(p.x >= 0.0F && p.x <= BOX_X);
        TEST_ASSERT_TRUE(p.y >= 0.0F && p.y <= BOX_Y);
        TEST_ASSERT_TRUE(p.z >= 0.0F && p.z <= BOX_Z);
    }
}

/* FLIP's divergence-free flow never closes a gap by itself; the two-way
 * drift push is what keeps a resting pool from slowly turning to foam, and
 * nothing packs it denser either. */
static void
test_a_resting_pool_keeps_its_volume(void) {
    fixture_begin();
    flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(SETTLE_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    const float settled = wet_share();
    TEST_ASSERT_FLOAT_WITHIN(0.03F, 0.5F, settled);
    run(LONG_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    TEST_ASSERT_FLOAT_WITHIN(0.02F, settled, wet_share());
}

static void
test_the_same_inputs_give_the_same_water(void) {
    fixture_begin();
    flip3_fill(&water, 0.4F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(60, (vec3f_t){GRAVITY, GRAVITY, 0.0F});
    vec3f_t* first = malloc(sizeof(vec3f_t) * (size_t)water.count);
    TEST_ASSERT_NOT_NULL(first);
    memcpy(first, water.pos, sizeof(vec3f_t) * (size_t)water.count);
    flip3_fill(&water, 0.4F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(60, (vec3f_t){GRAVITY, GRAVITY, 0.0F});
    const bool same = memcmp(first, water.pos, sizeof(vec3f_t) * (size_t)water.count) == 0;
    free(first);
    TEST_ASSERT_TRUE(same);
}

/* A split cut a third of the way and run later range first, as a host on
 * two cores may, with an empty range at either end, as a steered cut that
 * gives one core nothing runs. */
#define SPLIT_CUT_DIVISOR 3
static int splits_run;

static void
split_unevenly(int site, flip3_range_fn fn, void* ctx, int count) {
    (void)site;
    const int cut = count / SPLIT_CUT_DIVISOR;
    fn(ctx, count, count);
    fn(ctx, cut, count);
    fn(ctx, 0, cut);
    fn(ctx, 0, 0);
    splits_run++;
}

/* Tipped water stepped and its field taken, with or without the split;
 * the particles and the field kept in `out`. */
static void
tipped_water(flip3_split_fn split, bool red_black, void* out) {
    flip3_fill(&water, 0.4F, (vec3f_t){0.0F, 1.0F, 0.0F});
    flip3_split_with(&water, split);
    water.tuning.red_black = red_black;
    run(60, (vec3f_t){GRAVITY, GRAVITY, 0.0F});
    take_field();
    const size_t particles_bytes = sizeof(vec3f_t) * (size_t)water.count;
    memcpy(out, water.pos, particles_bytes);
    memcpy((char*)out + particles_bytes, water.vel, particles_bytes);
    memcpy((char*)out + (2 * particles_bytes), field, sizeof(float) * (size_t)(fx * fy * fz));
}

/* Every split writes disjoint items, so where it cuts and which part runs
 * first changes nothing, bit for bit, in index or red-black order. */
static void
test_a_split_anywhere_gives_the_same_water(void) {
    fixture_begin();
    const size_t bytes = (2 * sizeof(vec3f_t) * MAX_PARTICLES) + (sizeof(float) * flip3_field_count(&water, SAMPLES));
    char* whole = malloc(bytes);
    char* split = malloc(bytes);
    TEST_ASSERT_TRUE(whole != NULL && split != NULL);
    bool same = true;
    for (int red_black = 0; red_black < 2; red_black++) {
        memset(whole, 0, bytes);
        memset(split, 0, bytes);
        tipped_water(NULL, red_black != 0, whole);
        splits_run = 0;
        tipped_water(split_unevenly, red_black != 0, split);
        same = same && splits_run > 0 && memcmp(whole, split, bytes) == 0;
    }
    flip3_split_with(&water, NULL);
    free(whole);
    free(split);
    TEST_ASSERT_TRUE(same);
}

/* Red-black order relaxes the same equations in another order: the pool
 * still settles level and keeps its volume. */
static void
test_red_black_water_settles_level_and_keeps_its_volume(void) {
    fixture_begin();
    water.tuning.red_black = true;
    flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(SETTLE_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    TEST_ASSERT_LESS_THAN_FLOAT(CELL, rms_speed());
    TEST_ASSERT_FLOAT_WITHIN(0.03F, 0.5F, wet_share());
}

/* The device's box rounds each axis its own way; the water must still
 * settle level and keep its volume there. */
static void
test_a_box_of_no_whole_cells_settles_level_and_keeps_its_volume(void) {
    fixture_with(&ODD);
    flip3_fill(&water, 0.5F, (vec3f_t){0.0F, 1.0F, 0.0F});
    run(SETTLE_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    TEST_ASSERT_LESS_THAN_FLOAT(CELL, rms_speed());
    const float settled = wet_share();
    TEST_ASSERT_FLOAT_WITHIN(0.03F, 0.5F, settled);
    run(LONG_STEPS, (vec3f_t){0.0F, GRAVITY, 0.0F});
    TEST_ASSERT_FLOAT_WITHIN(0.02F, settled, wet_share());
}

void
suite_flip3(void) {
    RUN_TEST(test_a_bad_configuration_is_refused);
    RUN_TEST(test_whole_cells_fill_the_box_on_every_axis);
    RUN_TEST(test_a_fractional_sample_rate_rounds_each_axis_and_spans_the_box);
    RUN_TEST(test_a_fractional_rate_finds_the_same_level);
    RUN_TEST(test_a_fill_places_the_volume_asked_for);
    RUN_TEST(test_a_fill_starts_at_the_bottom_along_down);
    RUN_TEST(test_a_fill_is_capped_by_the_particles_there_is_room_for);
    RUN_TEST(test_upright_water_settles_level);
    RUN_TEST(test_flat_water_lies_as_a_layer_on_the_glass_below);
    RUN_TEST(test_tipped_toward_the_viewer_water_gathers_at_the_front);
    RUN_TEST(test_a_shaken_tank_keeps_every_particle_inside);
    RUN_TEST(test_a_resting_pool_keeps_its_volume);
    RUN_TEST(test_the_same_inputs_give_the_same_water);
    RUN_TEST(test_a_box_of_no_whole_cells_settles_level_and_keeps_its_volume);
    RUN_TEST(test_a_split_anywhere_gives_the_same_water);
    RUN_TEST(test_red_black_water_settles_level_and_keeps_its_volume);
}

SUITE_REGISTER(suite_flip3);
