#ifndef APP_SAND_TEST_H
#define APP_SAND_TEST_H

#include <stdbool.h>

typedef enum {
    SAND_TEST_START_WITHOUT_APPLY,
    SAND_TEST_CANCEL_THEN_START,
    SAND_TEST_APPLY_THEN_START,
} sand_test_start_action_t;

bool sand_app_test_options_reach_start(sand_test_start_action_t action);

#endif
