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

static void
test_checkpoint_bytes_counts_blocks_and_headers(void) {
    fixture();
    checkpoint_t saved = {0};
    char small[3] = {0};
    int large[5] = {0};
    TEST_ASSERT_EQUAL_size_t(0, checkpoint_bytes(&saved));
    CHECKPOINT_CAPTURE(&saved, small);
    CHECKPOINT_CAPTURE(&saved, large);
    TEST_ASSERT_EQUAL_size_t(2 * sizeof(checkpoint_block_t) + sizeof small + sizeof large, checkpoint_bytes(&saved));
    checkpoint_clear(&saved);
    TEST_ASSERT_EQUAL_size_t(0, checkpoint_bytes(&saved));
}

void
suite_checkpoint(void) {
    RUN_TEST(test_checkpoint_refuses_changed_allocation);
    RUN_TEST(test_checkpoint_bytes_counts_blocks_and_headers);
}

SUITE_REGISTER(suite_checkpoint);
