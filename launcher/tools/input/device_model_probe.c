/* device_model_probe: the device outline, the IMU's place and axes from board_geometry.h, and where
 * imu_axes_screen() sends each chip axis, as device vectors. */
#include "device_motion_probe.h"

int
main(void) {
    printf("{\"width\":%g,\"height\":%g,\"depth\":%g,\"corner\":%g,", (double)BOARD_DEVICE_WIDTH_MM,
           (double)BOARD_DEVICE_HEIGHT_MM, (double)BOARD_DEVICE_DEPTH_MM, (double)BOARD_DEVICE_CORNER_RADIUS_MM);
    printf("\"imu_centre\":[%g,%g,%g],\"imu_size\":[%g,%g,%g],\"chip_axes\":[", (double)BOARD_IMU_CENTRE_X_MM,
           (double)BOARD_IMU_CENTRE_Y_MM, (double)-(BOARD_DEVICE_DEPTH_MM + BOARD_IMU_SIZE_Z_MM / 2),
           (double)BOARD_IMU_SIZE_X_MM, (double)BOARD_IMU_SIZE_Y_MM, (double)BOARD_IMU_SIZE_Z_MM);
    for (int i = 0; i < 3; i++) {
        printf("%s[%g,%g,%g]", i ? "," : "", (double)probe_chip_axes[i][0], (double)probe_chip_axes[i][1],
               (double)probe_chip_axes[i][2]);
    }
    printf("],\"mapped_axes\":[");
    float mapped[3][3];
    probe_label_axes(mapped);
    for (int s = 0; s < 3; s++) {
        printf("%s[%g,%g,%g]", s ? "," : "", (double)mapped[s][0], (double)mapped[s][1], (double)mapped[s][2]);
    }
    puts("]}");
    return 0;
}
