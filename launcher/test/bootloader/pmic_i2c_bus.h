#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A simulated two-wire bus and a small AXP2101 stand-in, behind the
 * stubs/bootloader/ gpio_ll and esp_rom declarations. It reacts to the
 * shipped bit-banging like the real chip (an ACK, a shifted-out register),
 * then separately decodes the waveform - what a test asserts against. */

#define PMIC_BUS_SDA_PIN 15
#define PMIC_BUS_SCL_PIN 14

typedef enum {
    PMIC_BUS_TOKEN_START,
    PMIC_BUS_TOKEN_BYTE,
    PMIC_BUS_TOKEN_STOP,
} pmic_bus_token_kind_t;

typedef struct {
    pmic_bus_token_kind_t kind;
    uint8_t value;
    bool acked;
} pmic_bus_token_t;

void pmic_bus_reset(void);

void pmic_bus_set_reset_reason(int reason);
void pmic_bus_set_read_value(uint8_t value);
void pmic_bus_set_nack_at(int byte_index);

int pmic_bus_token_count(void);
const pmic_bus_token_t* pmic_bus_token(int index);
bool pmic_bus_saw_mid_byte_edge(void);

bool pmic_bus_pin_is_driven_low(uint32_t pin);
int pmic_bus_gpio_call_count(void);
bool pmic_bus_saw_delay_us(uint32_t us);
