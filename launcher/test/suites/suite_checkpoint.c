#include "core/checkpoint.h"
#include "suites.h"
#include "unity.h"

static void
fixture(void) {}

static void
test_checkpoint_refuses_changed_allocation(void) {
    fixture();
    checkpoint_t saved = {0};
    int value = 0;
    void* allocation = NULL;
    checkpoint_capture_at(&saved, &allocation, sizeof value);
    allocation = &value;
    TEST_ASSERT_FALSE(checkpoint_restore(&saved));
    checkpoint_clear(&saved);
}

void
suite_checkpoint(void) {
    RUN_TEST(test_checkpoint_refuses_changed_allocation);
}

SUITE_REGISTER(suite_checkpoint);
