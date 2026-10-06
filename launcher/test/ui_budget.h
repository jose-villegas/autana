/* Command-list headroom and reporting shared by screen budget suites. */
#pragma once

#include <stdio.h>

#include "ui/ui.h"
#include "unity.h"

#define COMMANDLIST_HEADROOM_BYTES 2048
#define COMMANDLIST_BUDGET         (MU_COMMANDLIST_SIZE - COMMANDLIST_HEADROOM_BYTES)

static inline int
ui_budget_end(void) {
    mu_end(ui_context());
    return ui_context()->command_list.idx;
}

static inline void
ui_budget_assert(const char* screen_name, int used) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s screen used %d of %d budget bytes (%d headroom)", screen_name, used,
             COMMANDLIST_BUDGET, MU_COMMANDLIST_SIZE - used);
    printf("%s\n", msg);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(COMMANDLIST_BUDGET, used, msg);
}
