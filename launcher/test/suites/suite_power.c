#include <string.h>
#include "console/verbs/console_battery_reply.h"
#include "power/power_cache.h"
#include "power/power_decode.h"
#include "suites.h"
#include "unity.h"

static int lock_depth;

static void
enter(void* context) {
    (void)context;
    TEST_ASSERT_EQUAL_INT(0, lock_depth++);
}

static void
leave(void* context) {
    (void)context;
    TEST_ASSERT_EQUAL_INT(1, lock_depth--);
}

static power_cache_t
fixture(void) {
    lock_depth = 0;
    return (power_cache_t){.enter = enter, .leave = leave};
}

static void
test_decode(void) {
    static const power_charge_t charges[] = {POWER_CHARGE_TRICKLE, POWER_CHARGE_PRE,    POWER_CHARGE_CC,
                                             POWER_CHARGE_CV,      POWER_CHARGE_DONE,   POWER_CHARGE_IDLE,
                                             POWER_CHARGE_UNKNOWN, POWER_CHARGE_UNKNOWN};
    static const power_flow_t flows[] = {POWER_FLOW_STANDBY, POWER_FLOW_CHARGING, POWER_FLOW_DISCHARGING,
                                         POWER_FLOW_UNKNOWN};
    power_registers_t regs = {.status = 0x28, .vbat_high = 0xcf, .vbat_low = 0x93, .percent = 82};
    for (int phase = 0; phase < 8; phase++) {
        for (int flow = 0; flow < 4; flow++) {
            regs.charge = (uint8_t)(phase | (flow << 5));
            power_battery_t b = power_decode(&regs, 123);
            TEST_ASSERT_TRUE(b.valid);
            TEST_ASSERT_TRUE(b.usb_present);
            TEST_ASSERT_TRUE(b.battery_present);
            TEST_ASSERT_EQUAL_INT(charges[phase], b.charge);
            TEST_ASSERT_EQUAL_INT(flows[flow], b.flow);
            TEST_ASSERT_EQUAL_UINT16(3987, b.battery_mv);
            TEST_ASSERT_EQUAL_UINT8(82, b.percent);
            TEST_ASSERT_EQUAL_INT64(123, b.read_us);
        }
    }
    regs.status = 0;
    regs.charge = 0x22;
    power_battery_t b = power_decode(&regs, 124);
    TEST_ASSERT_TRUE(b.valid);
    TEST_ASSERT_FALSE(b.battery_present);
    TEST_ASSERT_FALSE(b.usb_present);
    TEST_ASSERT_EQUAL_INT(0, b.battery_mv);
    TEST_ASSERT_EQUAL_INT(0, b.percent);
    TEST_ASSERT_EQUAL_INT(POWER_CHARGE_UNKNOWN, b.charge);
    TEST_ASSERT_EQUAL_INT(POWER_FLOW_UNKNOWN, b.flow);
    regs.status = 0x20;
    b = power_decode(&regs, 125);
    TEST_ASSERT_TRUE(b.usb_present);
    TEST_ASSERT_FALSE(b.battery_present);
}

static void
test_cache(void) {
    power_cache_t c = fixture();
    TEST_ASSERT_FALSE(power_cache_load(&c).valid);
    power_battery_t newer = {.valid = true,
                             .usb_present = true,
                             .battery_present = true,
                             .battery_mv = 3987,
                             .percent = 82,
                             .charge = POWER_CHARGE_CC,
                             .flow = POWER_FLOW_CHARGING,
                             .read_us = 20};
    power_battery_t older = {.valid = true, .read_us = 10};
    power_cache_store(&c, &newer);
    power_cache_store(&c, &older);
    power_battery_t b = power_cache_load(&c);
    TEST_ASSERT_TRUE(b.valid);
    TEST_ASSERT_EQUAL_INT64(20, b.read_us);
    TEST_ASSERT_EQUAL_UINT16(3987, b.battery_mv);
    TEST_ASSERT_EQUAL_UINT8(82, b.percent);
    TEST_ASSERT_TRUE(b.usb_present);
    TEST_ASSERT_TRUE(b.battery_present);
    TEST_ASSERT_EQUAL_INT(POWER_CHARGE_CC, b.charge);
    TEST_ASSERT_EQUAL_INT(POWER_FLOW_CHARGING, b.flow);
    b.battery_mv = 1;
    TEST_ASSERT_EQUAL_UINT16(3987, power_cache_load(&c).battery_mv);
    power_battery_t failure = {.read_us = 30};
    power_cache_store(&c, &failure);
    power_cache_store(&c, &newer);
    TEST_ASSERT_FALSE(power_cache_load(&c).valid);
    TEST_ASSERT_EQUAL_INT64(30, power_cache_load(&c).read_us);
    TEST_ASSERT_EQUAL_INT(0, lock_depth);
}

static void
test_reply(void) {
    char line[CONSOLE_BATTERY_REPLY_MAX];
    power_battery_t b = {.valid = true,
                         .usb_present = true,
                         .battery_present = true,
                         .battery_mv = 3987,
                         .percent = 82,
                         .charge = POWER_CHARGE_CC,
                         .flow = POWER_FLOW_CHARGING};
    power_registers_t regs = {.status = 0x2a, .charge = 0x22};
    console_battery_reply(line, sizeof line, &b, &regs);
    TEST_ASSERT_EQUAL_STRING(
        "BATTERY valid=1 usb=1 battery=1 mv=3987 percent=82 charge=cc flow=charging reg00=0x2a reg01=0x22", line);
    b = (power_battery_t){0};
    regs = (power_registers_t){0};
    console_battery_reply(line, sizeof line, &b, &regs);
    TEST_ASSERT_EQUAL_STRING(
        "BATTERY valid=0 usb=0 battery=0 mv=0 percent=0 charge=unknown flow=unknown reg00=0x00 reg01=0x00", line);
}

void
run_power_suite(void) {
    RUN_TEST(test_decode);
    RUN_TEST(test_cache);
    RUN_TEST(test_reply);
}

SUITE_REGISTER(run_power_suite);
