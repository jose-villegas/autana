/* board_size: the device's physical size, for anything drawn to scale.
 * Plain numbers, no vendor headers, so host builds read them too. */
#pragma once

/* Waveshare ESP32-S3-Touch-AMOLED-1.8: a 1.8 inch panel, corner to corner. */
#define BOARD_PANEL_DIAGONAL_MM 45.72F

/* Front glass to back, enclosure included. A placeholder until a caliper
 * reading replaces it. */
#define BOARD_THICKNESS_MM      12.0F
