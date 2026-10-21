#include <stdlib.h>
#include <string.h>
#include "console/verbs/console_battery_reply.h"
#include "power/power_log.h"
#include "suites.h"
#include "unity.h"

static void
test_log_pack(void) {
    power_log_record_t record = {.seq = POWER_LOG_SEQ_MAX,
                                 .up_min = POWER_LOG_UPTIME_MAX,
                                 .battery = {.battery_mv = POWER_LOG_MV_MAX,
                                             .percent = POWER_LOG_PERCENT_MAX,
                                             .usb_present = true,
                                             .battery_present = true,
                                             .charge = POWER_LOG_CHARGE_MAX,
                                             .flow = POWER_LOG_FLOW_MAX},
                                 .reason = POWER_LOG_REASON_MAX};
    power_log_record_t out = power_log_unpack(power_log_pack(&record));
    TEST_ASSERT_EQUAL_UINT32(record.seq, out.seq);
    TEST_ASSERT_EQUAL_UINT16(record.up_min, out.up_min);
    TEST_ASSERT_EQUAL_UINT16(record.battery.battery_mv, out.battery.battery_mv);
    TEST_ASSERT_EQUAL_UINT8(record.battery.percent, out.battery.percent);
    TEST_ASSERT_TRUE(out.battery.usb_present);
    TEST_ASSERT_TRUE(out.battery.battery_present);
    TEST_ASSERT_EQUAL_INT(record.battery.charge, out.battery.charge);
    TEST_ASSERT_EQUAL_INT(record.battery.flow, out.battery.flow);
    TEST_ASSERT_EQUAL_INT(record.reason, out.reason);
    record.seq++;
    record.up_min++;
    out = power_log_unpack(power_log_pack(&record));
    TEST_ASSERT_EQUAL_UINT32(0, out.seq);
    TEST_ASSERT_EQUAL_UINT16(POWER_LOG_UPTIME_MAX, out.up_min);
    TEST_ASSERT_EQUAL_UINT16(0, power_log_uptime(-1));
    TEST_ASSERT_EQUAL_UINT16(1, power_log_uptime(POWER_LOG_MINUTE_US));
    TEST_ASSERT_EQUAL_UINT16(POWER_LOG_UPTIME_MAX, power_log_uptime(INT64_MAX));
    char key[POWER_LOG_KEY_SIZE];
    power_log_key(key, 0);
    TEST_ASSERT_EQUAL_STRING("r000", key);
    power_log_key(key, POWER_LOG_SLOTS - 1);
    TEST_ASSERT_EQUAL_STRING("r359", key);
}

static void
test_log_ring(void) {
    power_log_slot_t* slots = calloc(POWER_LOG_SLOTS, sizeof *slots);
    unsigned* order = malloc(POWER_LOG_SLOTS * sizeof *order);
    TEST_ASSERT_NOT_NULL(slots);
    TEST_ASSERT_NOT_NULL(order);
    TEST_ASSERT_EQUAL_INT(-1, power_log_newest(slots));
    TEST_ASSERT_EQUAL_UINT(0, power_log_order(slots, order));
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        slots[i] = (power_log_slot_t){true, i};
    }
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SLOTS - 1, power_log_newest(slots));
    TEST_ASSERT_EQUAL_UINT(POWER_LOG_SLOTS, power_log_order(slots, order));
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        TEST_ASSERT_EQUAL_UINT(i, order[i]);
    }
    const unsigned rotation = POWER_LOG_SLOTS / 2;
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        slots[(i + rotation) % POWER_LOG_SLOTS].seq = i;
    }
    TEST_ASSERT_EQUAL_INT(rotation - 1, power_log_newest(slots));
    TEST_ASSERT_EQUAL_UINT(POWER_LOG_SLOTS, power_log_order(slots, order));
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        TEST_ASSERT_EQUAL_UINT((i + rotation) % POWER_LOG_SLOTS, order[i]);
    }
    for (unsigned i = 0; i < POWER_LOG_SLOTS; i++) {
        slots[i].seq = (POWER_LOG_SEQ_MAX - POWER_LOG_SLOTS / 2 + i) & POWER_LOG_SEQ_MAX;
        slots[i].present = i % 3 != 0;
    }
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SLOTS - 1, power_log_newest(slots));
    size_t count = power_log_order(slots, order);
    TEST_ASSERT_EQUAL_UINT(POWER_LOG_SLOTS - POWER_LOG_SLOTS / 3, count);
    for (size_t i = 1; i < count; i++) {
        TEST_ASSERT_TRUE(order[i] > order[i - 1]);
    }
    free(order);
    free(slots);
}

static void
test_log_policy(void) {
    power_battery_t before = {.valid = true, .battery_present = true};
    power_battery_t current = before;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_BOOT, power_log_decide(NULL, &current, 0, 0, NULL));
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&before, &current, POWER_LOG_PERIOD_US - 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT(POWER_LOG_PERIODIC, power_log_decide(&before, &current, POWER_LOG_PERIOD_US, 0, NULL));
    current.usb_present = true;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_TRANSITION, power_log_decide(&before, &current, 1, 0, NULL));
    before = current;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&before, &current, POWER_LOG_PERIOD_US, 0, NULL));
    current.battery_present = false;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_TRANSITION, power_log_decide(&before, &current, 1, 0, NULL));
    before = current;
    current.charge = POWER_CHARGE_CC;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_TRANSITION, power_log_decide(&before, &current, 1, 0, NULL));
    current.usb_present = false;
    before = current;
    const int64_t transition_us = POWER_LOG_PERIOD_US;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&before, &current, transition_us + 1, transition_us, NULL));
    TEST_ASSERT_EQUAL_INT(POWER_LOG_PERIODIC, power_log_decide(&before, &current, transition_us + POWER_LOG_PERIOD_US,
                                                               transition_us, NULL));
    current.valid = false;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(NULL, &current, 0, 0, NULL));
}

static void
test_log_warning_crossings(void) {
    power_log_warning_t warning = {.valid = true, .warning_percent = 15, .shutdown_percent = 1};
    power_battery_t battery = {.valid = true, .battery_present = true, .percent = warning.warning_percent + 1};
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent = warning.warning_percent;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SHUTDOWN_WARNING, power_log_decide(&battery, &battery, 1, 0, &warning));
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent--;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent = warning.shutdown_percent;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SHUTDOWN_WARNING, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent = 0;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
}

static void
test_log_warning_rearm(void) {
    power_log_warning_t warning = {
        .valid = true, .warning_percent = 15, .shutdown_percent = 1, .warning_logged = true, .shutdown_logged = true};
    power_battery_t battery = {.valid = true, .battery_present = true, .percent = warning.shutdown_percent + 1};
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent = warning.shutdown_percent;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SHUTDOWN_WARNING, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent = warning.warning_percent + 1;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
    battery.percent = warning.warning_percent;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SHUTDOWN_WARNING, power_log_decide(&battery, &battery, 1, 0, &warning));
}

static void
test_log_warning_usb(void) {
    power_log_warning_t warning = {
        .valid = true, .warning_percent = 15, .shutdown_percent = 1, .warning_logged = true, .shutdown_logged = true};
    power_battery_t battery = {.valid = true, .battery_present = true, .usb_present = true};
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, POWER_LOG_PERIOD_US, 0, &warning));
    TEST_ASSERT_FALSE(warning.warning_logged);
    TEST_ASSERT_FALSE(warning.shutdown_logged);
    battery.usb_present = false;
    TEST_ASSERT_EQUAL_INT(POWER_LOG_SHUTDOWN_WARNING, power_log_decide(&battery, &battery, 1, 0, &warning));
    TEST_ASSERT_EQUAL_INT(POWER_LOG_NONE, power_log_decide(&battery, &battery, 1, 0, &warning));
}

static void
test_log_console(void) {
    TEST_ASSERT_EQUAL_INT(CONSOLE_BATTERY_NOW, console_battery_parse(" "));
    TEST_ASSERT_EQUAL_INT(CONSOLE_BATTERY_LOG, console_battery_parse(" log "));
    TEST_ASSERT_EQUAL_INT(CONSOLE_BATTERY_CLEAR, console_battery_parse("LOG CLEAR"));
    TEST_ASSERT_EQUAL_INT(CONSOLE_BATTERY_CLEAR, console_battery_parse(" LOG	  clear "));
    const char* invalid[] = {"clear", "log extra", "log clear extra", "logs", "LOGCLEAR"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        TEST_ASSERT_EQUAL_INT(CONSOLE_BATTERY_INVALID, console_battery_parse(invalid[i]));
    }
    power_log_record_t record = {.seq = POWER_LOG_SEQ_MAX,
                                 .up_min = POWER_LOG_UPTIME_MAX,
                                 .battery = {.battery_mv = POWER_LOG_MV_MAX,
                                             .percent = POWER_LOG_PERCENT_MAX,
                                             .usb_present = true,
                                             .battery_present = true,
                                             .charge = POWER_CHARGE_CC,
                                             .flow = POWER_FLOW_CHARGING},
                                 .reason = POWER_LOG_BOOT};
    char line[CONSOLE_BATTERY_REPLY_MAX];
    console_battery_log_reply(line, sizeof line, &record);
    TEST_ASSERT_EQUAL_STRING(
        "BATLOG seq=16777215 up_min=1023 mv=16383 percent=127 usb=1 battery=1 charge=cc flow=charging reason=boot",
        line);
}

void
run_power_log_suite(void) {
    RUN_TEST(test_log_console);
    RUN_TEST(test_log_pack);
    RUN_TEST(test_log_ring);
    RUN_TEST(test_log_policy);
    RUN_TEST(test_log_warning_crossings);
    RUN_TEST(test_log_warning_rearm);
    RUN_TEST(test_log_warning_usb);
}

SUITE_REGISTER(run_power_log_suite);
