/* device_model_probe: the device outline, the IMU's place and axes from board_geometry.h, and where
 * imu_axes_screen() sends each chip axis, as device vectors. */
#include <stdio.h>

#include "board/board_geometry.h"
#include "input/motion/imu_sample.h"

static const float chip_axes[3][3] = {BOARD_IMU_AXIS_X, BOARD_IMU_AXIS_Y, BOARD_IMU_AXIS_Z};

int
main(void) {
    printf("{\"width\":%g,\"height\":%g,\"depth\":%g,\"corner\":%g,", (double)BOARD_DEVICE_WIDTH_MM,
           (double)BOARD_DEVICE_HEIGHT_MM, (double)BOARD_DEVICE_DEPTH_MM, (double)BOARD_DEVICE_CORNER_RADIUS_MM);
    printf("\"imu_centre\":[%g,%g,%g],\"imu_size\":[%g,%g,%g],\"chip_axes\":[", (double)BOARD_IMU_CENTRE_X_MM,
           (double)BOARD_IMU_CENTRE_Y_MM, (double)-(BOARD_DEVICE_DEPTH_MM + BOARD_IMU_SIZE_Z_MM / 2),
           (double)BOARD_IMU_SIZE_X_MM, (double)BOARD_IMU_SIZE_Y_MM, (double)BOARD_IMU_SIZE_Z_MM);
    for (int i = 0; i < 3; i++) {
        printf("%s[%g,%g,%g]", i ? "," : "", (double)chip_axes[i][0], (double)chip_axes[i][1], (double)chip_axes[i][2]);
    }
    printf("],\"mapped_axes\":[");
    for (int s = 0; s < 3; s++) {
        float v[3] = {0};
        for (int c = 0; c < 3; c++) {
            const vec3i_t unit = imu_axes_screen(c == 0, c == 1, c == 2);
            const int along = s == 0 ? unit.x : s == 1 ? unit.y : unit.z;
            for (int i = 0; i < 3; i++) {
                v[i] += (float)along * chip_axes[c][i];
            }
        }
        printf("%s[%g,%g,%g]", s ? "," : "", (double)v[0], (double)v[1], (double)v[2]);
    }
    puts("]}");
    return 0;
}
