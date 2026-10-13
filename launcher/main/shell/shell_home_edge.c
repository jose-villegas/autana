#include "shell/shell_home_edge.h"

/* The home swipe starts at the physical edge carrying the logical bottom.
 * suite_shell_home_edge.c proves this table against ui_transform_rect()
 * for every quarter turn of the logical canvas. */
gesture_edge_t
shell_exit_edge_for_quarter(int quarter) {
    static const gesture_edge_t edge_for_quarter[4] = {
        GESTURE_EDGE_BOTTOM, /* quarter 0: Portrait */
        GESTURE_EDGE_LEFT,   /* quarter 1: Landscape */
        GESTURE_EDGE_TOP,    /* quarter 2: Portrait upside down */
        GESTURE_EDGE_RIGHT,  /* quarter 3: Landscape upside down */
    };
    return edge_for_quarter[quarter];
}
