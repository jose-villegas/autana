/*
 * Host test for pmic_cold_boot.c, unchanged, against stubs/bootloader/.
 * pmic_i2c_bus.c plays the AXP2101 and decodes the waveform independently,
 * so an assertion reads the pins, not the model's bookkeeping.
 * bootloader_after_init() is the only entry point - a standalone binary.
 */

#include <stddef.h>
#include <stdio.h>

#include "pmic_i2c_bus.h"
#include "soc/reset_reasons.h"
#include "unity.h"

void bootloader_after_init(void);

#define PMIC_ADDR_WRITE 0x68
#define PMIC_ADDR_READ  0x69
#define PMIC_REG        0x10

void
setUp(void) {}

void
tearDown(void) {}

static void
fixture(void) {
    pmic_bus_reset();
}

static const pmic_bus_token_t*
find_last_byte_token(void) {
    const pmic_bus_token_t* last = NULL;
    for (int i = 0; i < pmic_bus_token_count(); i++) {
        const pmic_bus_token_t* t = pmic_bus_token(i);
        if (t->kind == PMIC_BUS_TOKEN_BYTE) {
            last = t;
        }
    }
    return last;
}

static void
assert_token(int index, pmic_bus_token_kind_t kind, uint8_t value, bool acked, const char* what) {
    const pmic_bus_token_t* t = pmic_bus_token(index);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kind, t->kind, what);
    if (kind == PMIC_BUS_TOKEN_BYTE) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(value, t->value, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(acked, t->acked, what);
    }
}

static void
test_power_off_bit_is_never_written_back(void) {
    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CORE_SW);
    pmic_bus_set_read_value(0xFF);
    bootloader_after_init();
    const pmic_bus_token_t* written = find_last_byte_token();
    TEST_ASSERT_NOT_NULL_MESSAGE(written, "a floating (0xFF) read must still produce a write-back byte");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFE, written->value, "bit 0 must be cleared and bit 1 set from a floating read");

    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CORE_SW);
    pmic_bus_set_read_value(0x05);
    bootloader_after_init();
    written = find_last_byte_token();
    TEST_ASSERT_NOT_NULL_MESSAGE(written, "a read with bit 0 set must still produce a write-back byte");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x06, written->value,
                                   "bit 0 must be cleared, bit 1 set, and the other read bits kept as read");
}

static void
test_successful_restart_matches_expected_i2c_transaction(void) {
    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CORE_SW);
    pmic_bus_set_read_value(0x04);

    bootloader_after_init();

    TEST_ASSERT_EQUAL_INT_MESSAGE(12, pmic_bus_token_count(), "a full restart is exactly 12 decoded tokens");
    assert_token(0, PMIC_BUS_TOKEN_START, 0, false, "opening START");
    assert_token(1, PMIC_BUS_TOKEN_BYTE, PMIC_ADDR_WRITE, true, "address, write direction");
    assert_token(2, PMIC_BUS_TOKEN_BYTE, PMIC_REG, true, "register pointer 0x10");
    assert_token(3, PMIC_BUS_TOKEN_START, 0, false, "repeated START, no STOP before it");
    assert_token(4, PMIC_BUS_TOKEN_BYTE, PMIC_ADDR_READ, true, "address, read direction");
    assert_token(5, PMIC_BUS_TOKEN_BYTE, 0x04, false, "the one read byte, master NACKs it");
    assert_token(6, PMIC_BUS_TOKEN_STOP, 0, false, "STOP after the read");
    assert_token(7, PMIC_BUS_TOKEN_START, 0, false, "fresh START for the write-back");
    assert_token(8, PMIC_BUS_TOKEN_BYTE, PMIC_ADDR_WRITE, true, "address, write direction again");
    assert_token(9, PMIC_BUS_TOKEN_BYTE, PMIC_REG, true, "register pointer 0x10 again");
    assert_token(10, PMIC_BUS_TOKEN_BYTE, 0x06, true, "the write-back value: bit 1 set, bit 0 clear");
    assert_token(11, PMIC_BUS_TOKEN_STOP, 0, false, "closing STOP");

    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_saw_mid_byte_edge(),
                              "SDA must change only while SCL is low, except at START/STOP");
    TEST_ASSERT_TRUE_MESSAGE(pmic_bus_saw_delay_us(20000), "a successful write must wait 20 ms before giving up");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SDA_PIN),
                              "SDA must be released when the hook returns");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SCL_PIN),
                              "SCL must be released when the hook returns");
}

static void
test_no_ack_on_address_byte_aborts_without_retry(void) {
    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CORE_SW);
    pmic_bus_set_nack_at(0);

    bootloader_after_init();

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, pmic_bus_token_count(), "a refused address byte sends nothing further");
    assert_token(0, PMIC_BUS_TOKEN_START, 0, false, "opening START");
    assert_token(1, PMIC_BUS_TOKEN_BYTE, PMIC_ADDR_WRITE, false, "the address byte, refused");
    assert_token(2, PMIC_BUS_TOKEN_STOP, 0, false, "STOP right after the refusal, no retry");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SDA_PIN), "SDA must still end released");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SCL_PIN), "SCL must still end released");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_saw_delay_us(20000), "an aborted transfer must not wait for a restart");
}

static void
test_no_ack_on_register_pointer_aborts_without_retry(void) {
    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CORE_SW);
    pmic_bus_set_nack_at(1);

    bootloader_after_init();

    TEST_ASSERT_EQUAL_INT_MESSAGE(4, pmic_bus_token_count(), "a refused register pointer sends nothing further");
    assert_token(0, PMIC_BUS_TOKEN_START, 0, false, "opening START");
    assert_token(1, PMIC_BUS_TOKEN_BYTE, PMIC_ADDR_WRITE, true, "the address byte, acked");
    assert_token(2, PMIC_BUS_TOKEN_BYTE, PMIC_REG, false, "the register pointer, refused");
    assert_token(3, PMIC_BUS_TOKEN_STOP, 0, false, "STOP right after the refusal, no retry");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SDA_PIN), "SDA must still end released");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SCL_PIN), "SCL must still end released");
}

static void
test_no_ack_on_data_byte_aborts_without_retry(void) {
    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CORE_SW);
    pmic_bus_set_read_value(0x00);
    pmic_bus_set_nack_at(5);

    bootloader_after_init();

    TEST_ASSERT_EQUAL_INT_MESSAGE(12, pmic_bus_token_count(),
                                  "the read phase still completes; only the final write is refused");
    assert_token(10, PMIC_BUS_TOKEN_BYTE, 0x02, false, "the write-back value, refused");
    assert_token(11, PMIC_BUS_TOKEN_STOP, 0, false, "STOP right after the refusal, no retry");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SDA_PIN), "SDA must still end released");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_pin_is_driven_low(PMIC_BUS_SCL_PIN), "SCL must still end released");
    TEST_ASSERT_FALSE_MESSAGE(pmic_bus_saw_delay_us(20000), "an aborted transfer must not wait for a restart");
}

static void
test_power_on_reset_does_not_touch_pmic_pins(void) {
    fixture();
    pmic_bus_set_reset_reason(RESET_REASON_CHIP_POWER_ON);

    bootloader_after_init();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pmic_bus_gpio_call_count(), "a power-on reset must not touch a GPIO at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pmic_bus_token_count(), "a power-on reset must not start a transaction");
}

static void
test_every_non_power_on_reset_reason_starts_a_transaction(void) {
    const int reasons[] = {
        RESET_REASON_CORE_USB_JTAG, RESET_REASON_CORE_SW,      RESET_REASON_CPU0_SW,
        RESET_REASON_CORE_MWDT0,    RESET_REASON_CORE_RTC_WDT, RESET_REASON_SYS_BROWN_OUT,
    };

    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        fixture();
        pmic_bus_set_reset_reason(reasons[i]);

        bootloader_after_init();

        TEST_ASSERT_TRUE_MESSAGE(pmic_bus_token_count() > 0, "this reset reason must start an I2C transaction");
        const pmic_bus_token_t* first = pmic_bus_token(0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(PMIC_BUS_TOKEN_START, first->kind, "the first thing on the wire must be a START");
    }
}

int
main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_power_off_bit_is_never_written_back);
    RUN_TEST(test_successful_restart_matches_expected_i2c_transaction);
    RUN_TEST(test_no_ack_on_address_byte_aborts_without_retry);
    RUN_TEST(test_no_ack_on_register_pointer_aborts_without_retry);
    RUN_TEST(test_no_ack_on_data_byte_aborts_without_retry);
    RUN_TEST(test_power_on_reset_does_not_touch_pmic_pins);
    RUN_TEST(test_every_non_power_on_reset_reason_starts_a_transaction);
    return UNITY_END();
}
