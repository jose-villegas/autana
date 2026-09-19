/*
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen_ui_layout.py main/ui/control_center_layout.json main/ui/control_center_layout_generated.h
 */
#pragma once

#include <stdint.h>

typedef enum {
    CONTROL_CENTER_ELEMENT_WIFI = 0,
    CONTROL_CENTER_ELEMENT_BLUETOOTH = 1,
    CONTROL_CENTER_ELEMENT_LINK = 2,
    CONTROL_CENTER_ELEMENT_VOLUME = 3,
    CONTROL_CENTER_ELEMENT_BRIGHTNESS = 4,
    CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER = 5,
    CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION = 6,
    CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION = 7,
    CONTROL_CENTER_ELEMENT_COUNT = 8
} control_center_element_id_t;

typedef struct {
    int16_t x, y, width, height;
} control_center_layout_rect_t;

typedef struct {
    int16_t canvas_width, canvas_height;
    control_center_layout_rect_t rects[CONTROL_CENTER_ELEMENT_COUNT];
} control_center_layout_t;

static const control_center_layout_t control_center_layout_portrait = {
    .canvas_width = 368,
    .canvas_height = 448,
    .rects =
        {
            [CONTROL_CENTER_ELEMENT_WIFI] = {16, 16, 160, 78},
            [CONTROL_CENTER_ELEMENT_BLUETOOTH] = {192, 16, 160, 78},
            [CONTROL_CENTER_ELEMENT_LINK] = {16, 106, 336, 58},
            [CONTROL_CENTER_ELEMENT_VOLUME] = {16, 176, 160, 52},
            [CONTROL_CENTER_ELEMENT_BRIGHTNESS] = {192, 176, 160, 52},
            [CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER] = {16, 242, 336, 18},
            [CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION] = {16, 270, 336, 72},
            [CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION] = {16, 354, 336, 72},
        },
};

static const control_center_layout_t control_center_layout_landscape = {
    .canvas_width = 448,
    .canvas_height = 368,
    .rects =
        {
            [CONTROL_CENTER_ELEMENT_WIFI] = {16, 16, 130, 84},
            [CONTROL_CENTER_ELEMENT_BLUETOOTH] = {159, 16, 130, 84},
            [CONTROL_CENTER_ELEMENT_LINK] = {302, 16, 130, 84},
            [CONTROL_CENTER_ELEMENT_VOLUME] = {16, 112, 203, 52},
            [CONTROL_CENTER_ELEMENT_BRIGHTNESS] = {229, 112, 203, 52},
            [CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER] = {16, 178, 416, 18},
            [CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION] = {16, 206, 416, 66},
            [CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION] = {16, 284, 416, 66},
        },
};
