/* motion_calibration_figures_probe: sample six scripted poses through the engine's rest windows and fit. */
#include <stdlib.h>

#include "device_motion_probe.h"
#include "input/motion/imu_calib.h"
#include "input/motion/imu_rest.h"
#include "math/linear/quatf.h"
#include "math/scalar/mathi.h"

#define PROBE_AXES            3
#define PROBE_POSES_PER_AXIS  2
#define PROBE_MATRIX_VALUES   (PROBE_AXES * PROBE_AXES)
#define PROBE_TURN_MS         900U
#define PROBE_FRAME_MS        150U
#define PROBE_DONE_MS         450U
#define PROBE_MG_PER_G        1000
#define PROBE_NOISE_G         0.004F
#define PROBE_NOISE_STEP      0.71F
#define PROBE_NOISE_AXIS_STEP 1.63F
/* Each raw sample, the rounded window mean and the fitted offset can each round by half a count. */
#define PROBE_FIT_TOLERANCE_G (PROBE_NOISE_G + 1.5 / INPUT_ACCEL_ONE_G + 1.0 / IMU_CALIB_SCALE_ONE)

static const probe_accel_error_t scripted = {.offset_g = {-0.060F, 0.105F, 0.080F}, .scale = {1.032F, 0.971F, 1.045F}};
static const float zero_rate_offset[PROBE_AXES] = {0};
static const char* const stage_names[] = {
    [IMU_REST_MOVING] = "moving",
    [IMU_REST_SETTLING] = "settling",
    [IMU_REST_MEASURING] = "measuring",
    [IMU_REST_DONE] = "done",
};

enum { POSE_X_POS, POSE_X_NEG, POSE_Y_POS, POSE_Y_NEG, POSE_Z_POS, POSE_Z_NEG, PROBE_POSES };

typedef struct {
    vec3i_t mean;
    unsigned settling, measuring, done;
} pose_result_t;

static quatf_t from, to;

static unsigned
ticks(unsigned ms) {
    return mathi_ceil_div(ms * MATH_US_PER_MS, INPUT_MOTION_SAMPLE_US);
}

static double
seconds(unsigned tick) {
    return (double)tick * INPUT_MOTION_SAMPLE_US / MATH_US_PER_S;
}

static probe_pose_t
pose_at(float t) {
    const float fraction = probe_ease(t, 0.0F, (float)seconds(ticks(PROBE_TURN_MS)));
    const quatf_t rotation = quatf_slerp(from, to, fraction);
    const vec3f_t x = quatf_rotate(rotation, vec3f_unit_x());
    const vec3f_t y = quatf_rotate(rotation, vec3f_unit_y());
    const vec3f_t z = quatf_rotate(rotation, vec3f_unit_z());
    return (probe_pose_t){{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}}};
}

static imu_calib_fit_t
fit_means(const pose_result_t results[PROBE_POSES]) {
    const vec3i_t up = {results[POSE_X_POS].mean.x, results[POSE_Y_POS].mean.y, results[POSE_Z_POS].mean.z};
    const vec3i_t down = {results[POSE_X_NEG].mean.x, results[POSE_Y_NEG].mean.y, results[POSE_Z_NEG].mean.z};
    return imu_calib_fit(up, down);
}

static void
print_vector(vec3i_t value, double factor) {
    printf("[%.9g,%.9g,%.9g]", value.x * factor, value.y * factor, value.z * factor);
}

static void
print_frame(unsigned tick, int pose, float t, input_motion_t sample, imu_rest_stage_t stage, int pairs,
            imu_calib_fit_t fit) {
    const probe_pose_t matrix = pose_at(t);
    const int32_t offset[] = {fit.calib.offset.x, fit.calib.offset.y, fit.calib.offset.z};
    printf("%s{\"t\":%.9g,\"pose_number\":%d,\"stage\":\"%s\",", tick ? "," : "", seconds(tick), pose + 1,
           stage_names[stage]);
    probe_print_pose(&matrix);
    printf(",\"acceleration\":");
    print_vector(sample.acceleration, 1.0 / INPUT_ACCEL_ONE_G);
    printf(",\"offset_mg\":[");
    for (int axis = 0; axis < PROBE_AXES; axis++) {
        printf("%s", axis ? "," : "");
        if (axis < pairs) {
            printf("%.9g", offset[axis] * ((double)PROBE_MG_PER_G / INPUT_ACCEL_ONE_G));
        } else {
            printf("null");
        }
    }
    printf("]}");
}

static bool
print_fit(imu_calib_fit_t fit) {
    const vec3i_t expected_offset = imu_axes_screen(lroundf(scripted.offset_g[0] * IMU_CALIB_SCALE_ONE),
                                                    lroundf(scripted.offset_g[1] * IMU_CALIB_SCALE_ONE),
                                                    lroundf(scripted.offset_g[2] * IMU_CALIB_SCALE_ONE));
    const vec3i_t signed_scale = imu_axes_screen(lroundf(scripted.scale[0] * IMU_CALIB_SCALE_ONE),
                                                 lroundf(scripted.scale[1] * IMU_CALIB_SCALE_ONE),
                                                 lroundf(scripted.scale[2] * IMU_CALIB_SCALE_ONE));
    const vec3i_t expected_scale = {abs(signed_scale.x), abs(signed_scale.y), abs(signed_scale.z)};
    const int32_t offset[] = {fit.calib.offset.x, fit.calib.offset.y, fit.calib.offset.z};
    const int32_t scale[] = {fit.scale_ppm.x, fit.scale_ppm.y, fit.scale_ppm.z};
    const int32_t offset_ppm[] = {expected_offset.x, expected_offset.y, expected_offset.z};
    const int32_t scale_ppm[] = {expected_scale.x, expected_scale.y, expected_scale.z};
    for (int axis = 0; axis < PROBE_AXES; axis++) {
        if (fabs((double)offset[axis] / INPUT_ACCEL_ONE_G - (double)offset_ppm[axis] / IMU_CALIB_SCALE_ONE)
                > PROBE_FIT_TOLERANCE_G
            || fabs((double)(scale[axis] - scale_ppm[axis]) / IMU_CALIB_SCALE_ONE) > PROBE_FIT_TOLERANCE_G) {
            fprintf(stderr, "motion_calibration_figures: fit misses the scripted error on %c\n", "xyz"[axis]);
            return false;
        }
    }
    printf("],\"fit\":{\"offset_mg\":");
    print_vector(fit.calib.offset, (double)PROBE_MG_PER_G / INPUT_ACCEL_ONE_G);
    printf(",\"scale_ppm\":");
    print_vector(fit.scale_ppm, 1.0);
    printf("},\"scripted\":{\"offset_mg\":");
    print_vector(expected_offset, (double)PROBE_MG_PER_G / IMU_CALIB_SCALE_ONE);
    printf(",\"scale_ppm\":");
    print_vector(expected_scale, 1.0);
    printf(",\"chip_offset_g\":[%g,%g,%g],\"chip_scale\":[%g,%g,%g],\"noise_g\":%g", (double)scripted.offset_g[0],
           (double)scripted.offset_g[1], (double)scripted.offset_g[2], (double)scripted.scale[0],
           (double)scripted.scale[1], (double)scripted.scale[2], (double)PROBE_NOISE_G);
    puts("}}");
    return true;
}

int
main(int argc, char** argv) {
    if (argc != 1 + PROBE_POSES * PROBE_MATRIX_VALUES) {
        fprintf(stderr, "motion_calibration_figures: expected %d pose matrices in column order\n", PROBE_POSES);
        return EXIT_FAILURE;
    }
    pose_result_t results[PROBE_POSES] = {0};
    imu_calib_fit_t fit = {0};
    const unsigned frame_ticks = ticks(PROBE_FRAME_MS);
    const float dt = (float)seconds(1);
    unsigned tick = 0;
    int pairs = 0;
    printf("{\"frames\":[");
    for (int pose = 0; pose < PROBE_POSES; pose++) {
        vec3f_t basis[PROBE_AXES];
        for (int axis = 0; axis < PROBE_AXES; axis++, argv += PROBE_AXES) {
            basis[axis] = (vec3f_t){strtof(argv[1], NULL), strtof(argv[2], NULL), strtof(argv[3], NULL)};
        }
        from = to;
        to = quatf_normalize(quatf_from_basis(basis[0], basis[1], basis[2]));
        if (pose == 0) {
            from = to;
        }
        const unsigned turn = pose == 0 ? 0 : ticks(PROBE_TURN_MS);
        const unsigned deadline = turn + ticks(IMU_REST_SETTLE_MS) + ticks(IMU_REST_WINDOW_MS) + 1;
        pose_result_t* result = &results[pose];
        imu_rest_t rest = {0};
        imu_rest_stage_t stage = IMU_REST_MOVING;
        for (unsigned local = 0;; local++, tick++) {
            const float t = (float)seconds(local);
            probe_accel_error_t noisy = scripted;
            for (int axis = 0; axis < PROBE_AXES; axis++) {
                noisy.offset_g[axis] += PROBE_NOISE_G * sinf(tick * PROBE_NOISE_STEP + axis * PROBE_NOISE_AXIS_STEP);
            }
            const input_motion_t sample = probe_motion_sample(pose_at, t, dt, zero_rate_offset, NULL, &noisy);
            if (local >= turn && stage != IMU_REST_DONE) {
                const imu_rest_stage_t previous = stage;
                stage = imu_rest_step(&rest, &sample, INPUT_MOTION_SAMPLE_US, true);
                if (stage == IMU_REST_SETTLING && stage != previous) {
                    result->settling = tick;
                }
                if (stage == IMU_REST_MEASURING && stage != previous) {
                    result->measuring = tick;
                }
                if (stage == IMU_REST_DONE) {
                    result->done = tick;
                    result->mean = imu_rest_mean(&rest).acceleration;
                    pairs = (pose + 1) / PROBE_POSES_PER_AXIS;
                    fit = fit_means(results);
                } else if (local >= deadline) {
                    fprintf(stderr, "motion_calibration_figures: rest window did not complete in pose %d\n", pose + 1);
                    return EXIT_FAILURE;
                }
            }
            if (tick % frame_ticks == 0) {
                print_frame(tick, pose, t, sample, stage, pairs, fit);
            }
            if (stage == IMU_REST_DONE && tick - result->done >= ticks(PROBE_DONE_MS)) {
                tick++;
                break;
            }
        }
    }
    printf("],\"poses\":[");
    for (int pose = 0; pose < PROBE_POSES; pose++) {
        const pose_result_t* result = &results[pose];
        printf("%s{\"settling\":%.9g,\"measuring\":%.9g,\"done\":%.9g,\"mean\":", pose ? "," : "",
               seconds(result->settling), seconds(result->measuring), seconds(result->done));
        print_vector(result->mean, 1.0 / INPUT_ACCEL_ONE_G);
        printf("}");
    }
    return print_fit(fit) ? EXIT_SUCCESS : EXIT_FAILURE;
}
