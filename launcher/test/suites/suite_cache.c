#include "suites.h"
#include "unity.h"

#include "build/build_variant.h"
#include "core/cache.h"

#if defined(DEVICE_BUILD) && !CONFIG_LAUNCHER_QEMU
#include "soc/extmem_reg.h"
#include "soc/soc.h"

#define CACHE_TEST_BYTES    128
#define CACHE_NESTED_OFFSET (CACHE_TEST_BYTES / 2)

static cache_autoload_state_t
cache_registers(void) {
    return (cache_autoload_state_t){
        .ctrl = REG_READ(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG),
        .addr = REG_READ(EXTMEM_DCACHE_AUTOLOAD_SCT0_ADDR_REG),
        .size = REG_READ(EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE_REG),
    };
}

static void
assert_cache_registers(cache_autoload_state_t expected, cache_autoload_state_t actual) {
    TEST_ASSERT_EQUAL_HEX32(expected.ctrl, actual.ctrl);
    TEST_ASSERT_EQUAL_HEX32(expected.addr, actual.addr);
    TEST_ASSERT_EQUAL_HEX32(expected.size, actual.size);
}

static void
test_nested_autoload_restores_registers(void) {
    static const uint8_t positions[CACHE_TEST_BYTES] = {0};
    const cache_autoload_state_t initial = cache_registers();
    TEST_ASSERT_BITS_LOW(EXTMEM_DCACHE_AUTOLOAD_ENA, initial.ctrl);
    const cache_autoload_state_t outer = cache_autoload_push(positions, sizeof positions);
    const cache_autoload_state_t outer_registers = cache_registers();
    const cache_autoload_state_t inner =
        cache_autoload_push(positions + CACHE_NESTED_OFFSET, sizeof positions - CACHE_NESTED_OFFSET);
    const cache_autoload_state_t inner_registers = cache_registers();
    cache_autoload_pop(inner);
    const cache_autoload_state_t restored_outer = cache_registers();
    cache_autoload_pop(outer);
    const cache_autoload_state_t restored_initial = cache_registers();

    assert_cache_registers(initial, outer);
    assert_cache_registers(outer_registers, inner);
    assert_cache_registers(outer_registers, restored_outer);
    assert_cache_registers(initial, restored_initial);
    TEST_ASSERT_BITS_LOW(EXTMEM_DCACHE_AUTOLOAD_ENA, restored_initial.ctrl);
    TEST_ASSERT_BITS_HIGH(EXTMEM_DCACHE_AUTOLOAD_ENA | EXTMEM_DCACHE_AUTOLOAD_SCT0_ENA, inner_registers.ctrl);
    TEST_ASSERT_BITS_LOW(EXTMEM_DCACHE_AUTOLOAD_SCT1_ENA, inner_registers.ctrl);
    TEST_ASSERT_EQUAL_HEX32((uintptr_t)(positions + CACHE_NESTED_OFFSET), inner_registers.addr);
    TEST_ASSERT_EQUAL_UINT32(sizeof positions - CACHE_NESTED_OFFSET, inner_registers.size);
}
#endif

void
run_cache_suite(void) {
#if defined(DEVICE_BUILD) && !CONFIG_LAUNCHER_QEMU
    RUN_TEST(test_nested_autoload_restores_registers);
#endif
}

SUITE_REGISTER(run_cache_suite);
