/* touch_report: decodes the controller's first contact and event flag. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/vec2i.h"

/* The board guarantees this report layout at TOUCH_REPORT_REG. */
#define TOUCH_REPORT_REG   0x02
#define TOUCH_REPORT_BYTES 5

typedef enum {
    TOUCH_EVENT_DOWN = 0,
    TOUCH_EVENT_UP = 1,
    TOUCH_EVENT_CONTACT = 2,
    TOUCH_EVENT_NONE = 3,
} touch_event_t;

typedef struct {
    uint8_t count;
    touch_event_t event;
    vec2i_t point;
} touch_report_t;

void touch_report_decode(const uint8_t raw[TOUCH_REPORT_BYTES], touch_report_t* out);
bool touch_report_has_point(const touch_report_t* report);
bool touch_report_lifted(const touch_report_t* report);
