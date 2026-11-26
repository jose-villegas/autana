/* The numbers behind light_probe_figures.py, through the runtime's own SH and probe-grid code. Prints JSON.
 *
 *     probe                    one probe projected from a sky, and an example grid's blend over a plane
 *     probe GRID.shpr [X,Y,Z ...]  a baked grid: each probe's irradiance along the six axes, then a lit
 *                                  sphere sampled from the grid at each point */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "math/scalar/math_const.h"
#include "math/sh/sh1.h"
#include "render/probe/sh_probes.h"
#include "render/skin/r3d_skin_light.h"

/* The sky a probe sees: a blue zenith fading to a warm ground bounce, plus a small bright sun. */
#define SKY_DIRECTIONS 4096
#define SUN_COS        0.995F /* the sun's angular radius, as the cosine against its direction */
static const vec3f_t SKY_ZENITH = {0.35F, 0.55F, 1.0F};
static const vec3f_t SKY_GROUND = {0.45F, 0.3F, 0.18F};
static const vec3f_t SUN_COLOUR = {40.0F, 36.0F, 30.0F};
static const vec3f_t SUN_TOWARD = {0.6F, 0.6F, 0.53F};
/* Pixels a side of each sphere picture. */
#define SPHERE_SIDE   64

/* The example grid: PROBES_X by 1 by PROBES_Z probes a unit apart, sampled over the plane a margin past them. */
#define PROBES_X      4
#define PROBES_Z      3
#define PROBE_SCALE   (1.0F / 1024.0F)
#define PLANE_MARGIN  0.75F
#define PLANE_SAMPLES 96
static const vec3f_t PROBE_COLOURS[PROBES_X * PROBES_Z] = {
    {0.9F, 0.2F, 0.2F}, {0.9F, 0.6F, 0.2F},   {0.9F, 0.9F, 0.2F}, {0.3F, 0.8F, 0.3F},
    {0.2F, 0.6F, 0.9F}, {0.25F, 0.25F, 0.3F}, {0.8F, 0.8F, 0.8F}, {0.6F, 0.3F, 0.8F},
    {0.2F, 0.8F, 0.8F}, {0.9F, 0.4F, 0.7F},   {0.5F, 0.5F, 0.2F}, {0.1F, 0.3F, 0.6F},
};
/* The point whose eight-corner weights the blend figure labels. */
static const vec3f_t BLEND_POINT = {1.3F, 0.0F, 0.6F};

static vec3f_t
sky(vec3f_t direction) {
    const float up = 0.5F * (direction.y + 1.0F);
    vec3f_t radiance = vec3f_add(vec3f_scale(SKY_ZENITH, up), vec3f_scale(SKY_GROUND, 1.0F - up));
    if (vec3f_dot(direction, vec3f_normalize(SUN_TOWARD)) > SUN_COS) {
        radiance = vec3f_add(radiance, SUN_COLOUR);
    }
    return radiance;
}

static vec3f_t
fibonacci(int i, int count) {
    const float polar = acosf(1.0F - (2.0F * ((float)i + 0.5F) / (float)count));
    const float azimuth = MATH_PI * (1.0F + sqrtf(5.0F)) * ((float)i + 0.5F);
    return (vec3f_t){cosf(azimuth) * sinf(polar), cosf(polar), sinf(azimuth) * sinf(polar)};
}

static vec3f_t directions[SKY_DIRECTIONS];
static vec3f_t radiances[SKY_DIRECTIONS];

/* Irradiance over pi at a normal, summed over the sky's directions: what the L1 evaluation approximates. */
static vec3f_t
exact(vec3f_t normal) {
    vec3f_t sum = vec3f_zero();
    for (int i = 0; i < SKY_DIRECTIONS; i++) {
        const float facing = vec3f_dot(normal, directions[i]);
        if (facing > 0.0F) {
            sum = vec3f_add(sum, vec3f_scale(radiances[i], facing));
        }
    }
    return vec3f_scale(sum, 4.0F / (float)SKY_DIRECTIONS);
}

static void
print_vec(vec3f_t v) {
    printf("[%.5g, %.5g, %.5g]", v.x, v.y, v.z);
}

/* Each picture is a sphere seen along -z: its pixels' normals, or null off the sphere. */
static void
print_sphere(const char* name, int what, const sh1_irradiance_t* ambient, const sh1_irradiance_t* flat) {
    printf("\"%s\": [", name);
    for (int row = 0; row < SPHERE_SIDE; row++) {
        printf("%s[", row ? ", " : "");
        for (int column = 0; column < SPHERE_SIDE; column++) {
            const float x = ((2.0F * ((float)column + 0.5F)) / SPHERE_SIDE) - 1.0F;
            const float y = 1.0F - ((2.0F * ((float)row + 0.5F)) / SPHERE_SIDE);
            const float rest = 1.0F - (x * x) - (y * y);
            printf("%s", column ? ", " : "");
            if (rest < 0.0F) {
                printf("null");
                continue;
            }
            const vec3f_t normal = {x, y, sqrtf(rest)};
            print_vec(what == 0   ? sky(normal)
                      : what == 1 ? exact(normal)
                      : what == 2 ? sh1_irradiance_eval(ambient, normal)
                                  : sh1_irradiance_eval(flat, normal));
        }
        printf("]");
    }
    printf("]");
}

static void
print_probe(void) {
    sh1_rgb_t radiance = {0};
    for (int i = 0; i < SKY_DIRECTIONS; i++) {
        directions[i] = fibonacci(i, SKY_DIRECTIONS);
        radiances[i] = sky(directions[i]);
        sh1_rgb_add_direction(&radiance, directions[i],
                              vec3f_scale(radiances[i], 4.0F * MATH_PI / (float)SKY_DIRECTIONS));
    }
    const sh1_irradiance_t ambient = r3d_skin_light_ambient_sh(&radiance);
    const sh1_irradiance_t flat = {.constant = ambient.constant};
    printf("\"coefficients\": [");
    for (int c = 0; c < SH1_COEFFICIENTS; c++) {
        printf("%s", c ? ", " : "");
        print_vec(radiance.c[c]);
    }
    printf("], ");
    print_sphere("sky", 0, &ambient, &flat);
    printf(", ");
    print_sphere("exact", 1, &ambient, &flat);
    printf(", ");
    print_sphere("l1", 2, &ambient, &flat);
    printf(", ");
    print_sphere("flat", 3, &ambient, &flat);
    /* A meridian from straight up to straight down, through the sun's side. */
    printf(", \"meridian\": [");
    const vec3f_t across = vec3f_normalize((vec3f_t){SUN_TOWARD.x, 0.0F, SUN_TOWARD.z});
    for (int degree = 0; degree <= 180; degree++) {
        const float angle = (float)degree * MATH_PI / 180.0F;
        const vec3f_t normal = vec3f_add(vec3f_scale(vec3f_unit_y(), cosf(angle)), vec3f_scale(across, sinf(angle)));
        printf("%s[%d, ", degree ? ", " : "", degree);
        print_vec(exact(normal));
        printf(", ");
        print_vec(sh1_irradiance_eval(&ambient, normal));
        printf(", ");
        print_vec(sh1_irradiance_eval(&flat, normal));
        printf("]");
    }
    printf("]");
}

static int16_t grid_coefficients[PROBES_X * PROBES_Z][SH_PROBES_COMPONENTS];

static sh_probes_t
grid(void) {
    return (sh_probes_t){.origin = vec3f_zero(),
                         .spacing = {1.0F, 1.0F, 1.0F},
                         .dims = {PROBES_X, 1, PROBES_Z},
                         .scale = PROBE_SCALE,
                         .coefficients = (const int16_t (*)[SH_PROBES_COMPONENTS])grid_coefficients};
}

/* Store each probe's constant band so its irradiance over pi is `colour`; an all-zero grid but for one probe
 * at one makes the sample that probe's weight. */
static void
fill(int only) {
    const float to_constant = 1.0F / (SH1_A0 * SH1_Y00 * R3D_SKIN_LIGHT_LAMBERT_SCALE);
    for (int p = 0; p < PROBES_X * PROBES_Z; p++) {
        const vec3f_t colour = only < 0 ? PROBE_COLOURS[p] : (vec3f_t){p == only, p == only, p == only};
        const float channels[SH1_CHANNELS] = {colour.x, colour.y, colour.z};
        for (int channel = 0; channel < SH1_CHANNELS; channel++) {
            grid_coefficients[p][channel] = (int16_t)lroundf(channels[channel] * to_constant / PROBE_SCALE);
        }
    }
}

static vec3f_t
sampled(const sh_probes_t* probes, vec3f_t at) {
    const sh1_rgb_t radiance = sh_probes_sample(probes, at);
    return r3d_skin_light_ambient_sh(&radiance).constant;
}

static void
print_blend(void) {
    const sh_probes_t probes = grid();
    fill(-1);
    printf("\"grid\": {\"x\": %d, \"z\": %d, \"bytes_per_probe\": %zu, \"probes\": [", PROBES_X, PROBES_Z,
           sizeof grid_coefficients[0]);
    for (int p = 0; p < PROBES_X * PROBES_Z; p++) {
        printf("%s", p ? ", " : "");
        print_vec(sampled(&probes, (vec3f_t){(float)(p % PROBES_X), 0.0F, (float)(p / PROBES_X)}));
    }
    printf("], \"margin\": %g, \"plane\": [", PLANE_MARGIN);
    for (int row = 0; row < PLANE_SAMPLES; row++) {
        printf("%s[", row ? ", " : "");
        for (int column = 0; column < PLANE_SAMPLES; column++) {
            const float x =
                -PLANE_MARGIN + ((PROBES_X - 1 + 2 * PLANE_MARGIN) * ((float)column + 0.5F) / PLANE_SAMPLES);
            const float z = -PLANE_MARGIN + ((PROBES_Z - 1 + 2 * PLANE_MARGIN) * ((float)row + 0.5F) / PLANE_SAMPLES);
            printf("%s", column ? ", " : "");
            print_vec(sampled(&probes, (vec3f_t){x, 0.0F, z}));
        }
        printf("]");
    }
    printf("], \"point\": ");
    print_vec(BLEND_POINT);
    printf(", \"weights\": [");
    for (int p = 0; p < PROBES_X * PROBES_Z; p++) {
        fill(p);
        printf("%s%.4g", p ? ", " : "", sampled(&probes, BLEND_POINT).x);
    }
    printf("]}");
}

/* SHPR v1 (docs/assets/formats/SHPR.md): the header sh_probes.h lays out, the records, then the inside mask. */
#define AXES 3
static const vec3f_t AXIS_NORMALS[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
#define AXIS_NORMAL_COUNT ((int)(sizeof AXIS_NORMALS / sizeof AXIS_NORMALS[0]))

static unsigned char*
read_file(const char* path, long* size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (*size = ftell(file)) < (long)SH_PROBES_HEADER_SIZE
        || fseek(file, 0, SEEK_SET) != 0) {
        exit(1);
    }
    unsigned char* bytes = malloc((size_t)*size);
    if (bytes == NULL || fread(bytes, 1, (size_t)*size, file) != (size_t)*size) {
        exit(1);
    }
    fclose(file);
    return bytes;
}

static sh_probes_t
load_grid(const unsigned char* bytes, long size) {
    sh_probes_t probes;
    if (sh_probes_read((asset_view_t){bytes, (uint32_t)size}, &probes) != ASSET_OK) {
        exit(1);
    }
    return probes;
}

/* Each probe's irradiance over pi along the six axes, x fastest, then a lit sphere at each "X,Y,Z" point. */
static void
print_grid(const sh_probes_t* probes, int points, char** point) {
    printf("\"origin\": ");
    print_vec(probes->origin);
    printf(", \"spacing\": ");
    print_vec(probes->spacing);
    printf(", \"dims\": [%d, %d, %d], \"bytes_per_probe\": %zu, \"irradiance\": [", probes->dims[0], probes->dims[1],
           probes->dims[2], sizeof probes->coefficients[0]);
    int printed = 0;
    for (int z = 0; z < probes->dims[2]; z++) {
        for (int y = 0; y < probes->dims[1]; y++) {
            for (int x = 0; x < probes->dims[0]; x++) {
                const vec3f_t at =
                    vec3f_add(probes->origin, (vec3f_t){(float)x * probes->spacing.x, (float)y * probes->spacing.y,
                                                        (float)z * probes->spacing.z});
                const sh1_rgb_t radiance = sh_probes_sample(probes, at);
                const sh1_irradiance_t ambient = r3d_skin_light_ambient_sh(&radiance);
                printf("%s[", printed++ ? ", " : "");
                for (int n = 0; n < AXIS_NORMAL_COUNT; n++) {
                    printf("%s", n ? ", " : "");
                    print_vec(sh1_irradiance_eval(&ambient, AXIS_NORMALS[n]));
                }
                printf("]");
            }
        }
    }
    printf("], \"spheres\": [");
    for (int i = 0; i < points; i++) {
        vec3f_t at;
        if (sscanf(point[i], "%f,%f,%f", &at.x, &at.y, &at.z) != AXES) {
            exit(1);
        }
        const sh1_rgb_t radiance = sh_probes_sample(probes, at);
        const sh1_irradiance_t ambient = r3d_skin_light_ambient_sh(&radiance);
        printf("%s{", i ? ", " : "");
        print_sphere("lit", 2, &ambient, &ambient);
        printf("}");
    }
    printf("]");
}

int
main(int argc, char** argv) {
    printf("{");
    if (argc > 1) {
        long size = 0;
        unsigned char* bytes = read_file(argv[1], &size);
        const sh_probes_t probes = load_grid(bytes, size);
        print_grid(&probes, argc - 2, argv + 2);
        free(bytes);
    } else {
        print_probe();
        printf(", ");
        print_blend();
    }
    printf("}\n");
    return 0;
}
