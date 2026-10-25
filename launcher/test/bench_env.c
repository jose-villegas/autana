/* bench_env: engine identity and frame-counter ownership for benchmark suites. */
#include "esp_cpu.h"
#include "profile/frame_cost.h"
#include "services/build_id.h"
#include "testkit/bench.h"
#include "unity.h"

const char*
bench_build_id(void) {
    return build_id();
}

bool
bench_counters_idle(void) {
    return frame_cost_counters_idle();
}

bool
bench_insn_begin(void) {
    TEST_ASSERT_EQUAL_INT(0, esp_cpu_get_core_id());
    TEST_ASSERT_TRUE(bench_counters_idle());
    const int event = frame_cost_event_index(FRAME_COST_DEFAULT_EVENT);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, event);
    TEST_ASSERT_TRUE(frame_cost_count_begin(event));
    return true;
}

bool
bench_insn_end(uint32_t* insn) {
    uint32_t cycles;
    return frame_cost_count_end(&cycles, insn);
}
