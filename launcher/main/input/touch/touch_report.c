#include "input/touch/touch_report.h"

enum {
    REPORT_COUNT,
    REPORT_XH,
    REPORT_XL,
    REPORT_YH,
    REPORT_YL,
};

#define REPORT_NIBBLE_MASK 0x0FU
#define REPORT_EVENT_SHIFT 6
#define REPORT_COORD_SHIFT 8
#define REPORT_MAX_POINTS  5

void
touch_report_decode(const uint8_t raw[TOUCH_REPORT_BYTES], touch_report_t* out) {
    out->count = raw[REPORT_COUNT] & REPORT_NIBBLE_MASK;
    out->event = (touch_event_t)(raw[REPORT_XH] >> REPORT_EVENT_SHIFT);
    out->point.x = (int)((raw[REPORT_XH] & REPORT_NIBBLE_MASK) << REPORT_COORD_SHIFT) | raw[REPORT_XL];
    out->point.y = (int)((raw[REPORT_YH] & REPORT_NIBBLE_MASK) << REPORT_COORD_SHIFT) | raw[REPORT_YL];
}

bool
touch_report_has_point(const touch_report_t* report) {
    return report->count > 0 && report->count <= REPORT_MAX_POINTS;
}

bool
touch_report_lifted(const touch_report_t* report) {
    return report->count == 0 && report->event == TOUCH_EVENT_UP;
}
