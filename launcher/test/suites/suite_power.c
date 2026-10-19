#include <limits.h>
#include <string.h>
#include "console/verbs/console_battery_reply.h"
#include "power/axp2101_regs.h"
#include "power/power_cache.h"
#include "power/power_decode.h"
#include "suites.h"
#include "unity.h"

static int lock_depth;
static int enters;
static int leaves;

static void
enter(void* context) {
    (void)context;
    enters++;
    TEST_ASSERT_EQUAL_INT(0, lock_depth++);
}

static void
leave(void* context) {
    (void)context;
    leaves++;
    TEST_ASSERT_EQUAL_INT(1, lock_depth--);
}

static power_cache_t
fixture(void) {
    lock_depth = 0;
    enters = leaves = 0;
    return (power_cache_t){.enter = enter, .leave = leave};
}

static void
test_decode(void) {
    static const struct {
        uint8_t code;
        power_charge_t state;
    } charges[] = {{0x00, POWER_CHARGE_TRICKLE}, {0x01, POWER_CHARGE_PRE},    {0x02, POWER_CHARGE_CC},
                   {0x03, POWER_CHARGE_CV},      {0x04, POWER_CHARGE_DONE},   {0x05, POWER_CHARGE_IDLE},
                   {0x06, POWER_CHARGE_UNKNOWN}, {0x07, POWER_CHARGE_UNKNOWN}};

    static const struct {
        uint8_t code;
        power_flow_t state;
    } flows[] = {{0x00, POWER_FLOW_STANDBY},
                 {0x20, POWER_FLOW_CHARGING},
                 {0x40, POWER_FLOW_DISCHARGING},
                 {0x60, POWER_FLOW_UNKNOWN}};

    power_registers_t regs = {.status = 0x28, .vbat_high = 0xcf, .vbat_low = 0x93, .percent = 82};
    for (int phase = 0; phase < 8; phase++) {
        for (int flow = 0; flow < 4; flow++) {
            regs.charge = (uint8_t)(charges[phase].code | flows[flow].code);
            power_battery_t b = power_decode(&regs, 123);
            TEST_ASSERT_TRUE(b.valid);
            TEST_ASSERT_TRUE(b.usb_present);
            TEST_ASSERT_TRUE(b.battery_present);
            TEST_ASSERT_EQUAL_INT(charges[phase].state, b.charge);
            TEST_ASSERT_EQUAL_INT(flows[flow].state, b.flow);
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

static void
test_percent_clamp(void) {
    const uint8_t inputs[] = {AXP2101_PERCENT_MAX, AXP2101_PERCENT_MAX + 1, UINT8_MAX, 0};
    const uint8_t expected[] = {AXP2101_PERCENT_MAX, AXP2101_PERCENT_MAX, AXP2101_PERCENT_MAX, 0};
    for (size_t i = 0; i < sizeof inputs; i++) {
        power_registers_t regs = {.status = AXP2101_BATTERY_PRESENT, .percent = inputs[i]};
        TEST_ASSERT_EQUAL_UINT8(expected[i], power_decode(&regs, 0).percent);
    }
}

static void
test_vbat_bits(void) {
    const uint8_t high[] = {AXP2101_VBAT_HIGH_MASK, 0xbf, 0x7f, UINT8_MAX, 0x80, 0x40, 0};
    const uint16_t max_mv = (AXP2101_VBAT_HIGH_MASK << AXP2101_VBAT_HIGH_SHIFT) | UINT8_MAX;
    const uint16_t expected[] = {max_mv, max_mv, max_mv, max_mv, UINT8_MAX, UINT8_MAX, UINT8_MAX};
    for (size_t i = 0; i < sizeof high; i++) {
        power_registers_t regs = {.status = AXP2101_BATTERY_PRESENT, .vbat_high = high[i], .vbat_low = UINT8_MAX};
        TEST_ASSERT_EQUAL_UINT16(expected[i], power_decode(&regs, 0).battery_mv);
    }
}

static void
test_status_bits(void) {
    power_registers_t regs = {
        .status = AXP2101_BATTERY_PRESENT, .vbat_high = UINT8_MAX, .vbat_low = UINT8_MAX, .percent = UINT8_MAX};
    power_battery_t b = power_decode(&regs, 0);
    TEST_ASSERT_TRUE(b.battery_present);
    TEST_ASSERT_FALSE(b.usb_present);
    regs.status = (uint8_t)~(AXP2101_BATTERY_PRESENT | AXP2101_USB_GOOD);
    for (uint8_t code = 0; code <= AXP2101_CHARGE_MASK; code++) {
        regs.charge = code;
        b = power_decode(&regs, 0);
        TEST_ASSERT_FALSE(b.battery_present);
        TEST_ASSERT_FALSE(b.usb_present);
        TEST_ASSERT_EQUAL_UINT16(0, b.battery_mv);
        TEST_ASSERT_EQUAL_UINT8(0, b.percent);
        TEST_ASSERT_EQUAL_INT(POWER_CHARGE_UNKNOWN, b.charge);
        TEST_ASSERT_EQUAL_INT(POWER_FLOW_UNKNOWN, b.flow);
    }
}

static void
test_cache_hooks_and_ordering(void) {
    power_cache_t c = fixture();
    power_battery_t good = {.valid = true, .read_us = 20};
    power_battery_t failed = {.read_us = 10};
    for (int call = 1; call <= 3; call++) {
        power_cache_store(&c, call == 1 ? &good : &failed);
        TEST_ASSERT_EQUAL_INT(call, enters);
        TEST_ASSERT_EQUAL_INT(call, leaves);
    }
    TEST_ASSERT_TRUE(power_cache_load(&c).valid);
    TEST_ASSERT_EQUAL_INT(4, enters);
    TEST_ASSERT_EQUAL_INT(4, leaves);
    failed.read_us = good.read_us;
    power_cache_store(&c, &failed);
    TEST_ASSERT_EQUAL_INT(5, enters);
    TEST_ASSERT_EQUAL_INT(5, leaves);
    TEST_ASSERT_FALSE(power_cache_load(&c).valid);
    TEST_ASSERT_EQUAL_INT(6, enters);
    TEST_ASSERT_EQUAL_INT(6, leaves);
}

static void
test_reply_capacity_and_names(void) {
    char line[CONSOLE_BATTERY_REPLY_MAX];
    power_battery_t b = {.valid = true,
                         .usb_present = true,
                         .battery_present = true,
                         .battery_mv = UINT16_MAX,
                         .percent = UINT8_MAX,
                         .charge = POWER_CHARGE_UNKNOWN,
                         .flow = POWER_FLOW_DISCHARGING};
    power_registers_t regs = {.status = UINT8_MAX, .charge = UINT8_MAX};
    console_battery_reply(line, sizeof line, &b, &regs);
    TEST_ASSERT_EQUAL_STRING(
        "BATTERY valid=1 usb=1 battery=1 mv=65535 percent=255 charge=unknown flow=discharging reg00=0xff reg01=0xff",
        line);
    static const char* const charges[] = {"unknown", "trickle", "pre", "cc", "cv", "done", "idle"};
    static const char* const flows[] = {"unknown", "standby", "charging", "discharging"};
    for (int i = POWER_CHARGE_UNKNOWN; i <= POWER_CHARGE_IDLE; i++) {
        TEST_ASSERT_EQUAL_STRING(charges[i], power_charge_name((power_charge_t)i));
    }
    for (int i = POWER_FLOW_UNKNOWN; i <= POWER_FLOW_DISCHARGING; i++) {
        TEST_ASSERT_EQUAL_STRING(flows[i], power_flow_name((power_flow_t)i));
    }
    TEST_ASSERT_EQUAL_STRING(charges[0], power_charge_name((power_charge_t)-1));
    TEST_ASSERT_EQUAL_STRING(flows[0], power_flow_name((power_flow_t)-1));
}

void
run_power_suite(void) {
    RUN_TEST(test_decode);
    RUN_TEST(test_percent_clamp);
    RUN_TEST(test_vbat_bits);
    RUN_TEST(test_status_bits);
    RUN_TEST(test_cache_hooks_and_ordering);
    RUN_TEST(test_reply_capacity_and_names);
    RUN_TEST(test_cache);
    RUN_TEST(test_reply);
}

SUITE_REGISTER(run_power_suite);
