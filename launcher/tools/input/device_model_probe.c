/* device_model_probe: the device outline, the IMU's place and axes from board_geometry.h, and where
 * imu_axes_screen() sends each chip axis, as device vectors. */
#include "device_motion_probe.h"
#include "display/display.h"
#include "gfx/gfx.h"

int
main(void) {
    printf("{\"width\":%g,\"height\":%g,\"depth\":%g,\"corner\":%g,", (double)BOARD_DEVICE_WIDTH_MM,
           (double)BOARD_DEVICE_HEIGHT_MM, (double)BOARD_DEVICE_DEPTH_MM, (double)BOARD_DEVICE_CORNER_RADIUS_MM);
    printf("\"case_width\":%g,\"case_height\":%g,\"case_depth\":%g,\"case_corner\":%g,", (double)BOARD_CASE_WIDTH_MM,
           (double)BOARD_CASE_HEIGHT_MM, (double)BOARD_CASE_DEPTH_MM, (double)BOARD_CASE_CORNER_RADIUS_MM);
    printf("\"screen_width\":%g,\"screen_height\":%g,\"back_side\":%g,\"screw_radius\":%g,",
           (double)BOARD_SCREEN_WIDTH_MM, (double)BOARD_SCREEN_HEIGHT_MM, (double)BOARD_CASE_BACK_SIDE_MM,
           (double)BOARD_CASE_SCREW_RADIUS_MM);
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
    printf("],\"screen_corner\":%g,\"side_features\":[",
           (double)(DISPLAY_PANEL_CORNER_RADIUS * BOARD_SCREEN_WIDTH_MM / GFX_WIDTH));

    static const struct {
        const char* side;
        const char* shape;
        float y, z, size_y, size_z, proud;
    } features[] = {
        {"right", "box", BOARD_BOOT_BUTTON_Y_MM, BOARD_BUTTON_CENTRE_Z_MM, BOARD_BUTTON_SIZE_Y_MM,
         BOARD_BUTTON_SIZE_Z_MM, BOARD_BUTTON_PROUD_MM},
        {"right", "box", BOARD_PWR_BUTTON_Y_MM, BOARD_BUTTON_CENTRE_Z_MM, BOARD_BUTTON_SIZE_Y_MM,
         BOARD_BUTTON_SIZE_Z_MM, BOARD_BUTTON_PROUD_MM},
        {"right", "hole", BOARD_USB_C_Y_MM, BOARD_USB_C_CENTRE_Z_MM, BOARD_USB_C_SIZE_Y_MM, BOARD_USB_C_SIZE_Z_MM, 0},
        {"left", "hole", BOARD_SPEAKER_PORT_Y_MM, BOARD_SPEAKER_PORT_Z_MM, BOARD_SPEAKER_PORT_DIAMETER_MM,
         BOARD_SPEAKER_PORT_DIAMETER_MM, 0},
        {"left", "hole", BOARD_SPEAKER_VENT_Y_MM, BOARD_SPEAKER_VENT_Z_MM, BOARD_SPEAKER_VENT_SIZE_Y_MM,
         BOARD_SPEAKER_VENT_SIZE_Z_MM, 0},
        {"left", "hole", BOARD_SD_SLOT_Y_MM, BOARD_SD_SLOT_Z_MM, BOARD_SD_SLOT_SIZE_Y_MM, BOARD_SD_SLOT_SIZE_Z_MM, 0},
        {"left", "hole", BOARD_MIC_PORT_Y_MM, BOARD_MIC_PORT_Z_MM, BOARD_MIC_PORT_DIAMETER_MM,
         BOARD_MIC_PORT_DIAMETER_MM, 0},
    };

    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
        printf("%s{\"side\":\"%s\",\"shape\":\"%s\",\"y\":%g,\"z\":%g,\"size_y\":%g,\"size_z\":%g,\"proud\":%g}",
               i ? "," : "", features[i].side, features[i].shape, (double)features[i].y, (double)features[i].z,
               (double)features[i].size_y, (double)features[i].size_z, (double)features[i].proud);
    }
    puts("]}");
    return 0;
}
