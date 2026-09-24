#include "pmic_i2c_bus.h"

#include <stddef.h>

#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/reset_reasons.h"

gpio_dev_t GPIO;

#define TOKEN_MAX 32

static bool master_sda_low;
static bool master_scl_low;
static bool slave_sda_low;

static bool decoding;
static int decode_bit_index;
static uint8_t decode_byte_bits;
static bool saw_mid_byte_edge;
static bool pending_sample_valid;
static bool pending_bit_is_one;

static pmic_bus_token_t tokens[TOKEN_MAX];
static int token_count;

static bool expect_address;
static bool current_byte_slave_driven;
static bool next_byte_slave_driven;
static int master_byte_index;
static int nack_at;
static uint8_t read_value;

/* Only the distinct delay values matter to a test - I2C_HALF_US repeats a
 * couple hundred times per transfer, the 20 ms post-restart wait exactly
 * once - so this dedups rather than logging every call. */
#define DISTINCT_DELAYS_MAX 8
static uint32_t distinct_delays[DISTINCT_DELAYS_MAX];
static int distinct_delay_count;

static int gpio_calls;
static int reset_reason;

static void slave_prepare_next_bit(void);
static void slave_on_8_bits_clocked(void);

void
pmic_bus_reset(void) {
    master_sda_low = false;
    master_scl_low = false;
    slave_sda_low = false;

    decoding = false;
    decode_bit_index = 0;
    decode_byte_bits = 0;
    saw_mid_byte_edge = false;
    pending_sample_valid = false;
    pending_bit_is_one = false;

    token_count = 0;

    expect_address = false;
    current_byte_slave_driven = false;
    next_byte_slave_driven = false;
    master_byte_index = 0;
    nack_at = -1;
    read_value = 0xFF;

    distinct_delay_count = 0;
    gpio_calls = 0;
    reset_reason = 0;
}

void
pmic_bus_set_reset_reason(int reason) {
    reset_reason = reason;
}

void
pmic_bus_set_read_value(uint8_t value) {
    read_value = value;
}

void
pmic_bus_set_nack_at(int byte_index) {
    nack_at = byte_index;
}

int
pmic_bus_token_count(void) {
    return token_count;
}

const pmic_bus_token_t*
pmic_bus_token(int index) {
    if (index < 0 || index >= token_count) {
        return NULL;
    }
    return &tokens[index];
}

bool
pmic_bus_saw_mid_byte_edge(void) {
    return saw_mid_byte_edge;
}

bool
pmic_bus_pin_is_driven_low(uint32_t pin) {
    return pin == PMIC_BUS_SDA_PIN ? master_sda_low : master_scl_low;
}

int
pmic_bus_gpio_call_count(void) {
    return gpio_calls;
}

bool
pmic_bus_saw_delay_us(uint32_t us) {
    for (int i = 0; i < distinct_delay_count; i++) {
        if (distinct_delays[i] == us) {
            return true;
        }
    }
    return false;
}

static void
append_token(pmic_bus_token_kind_t kind, uint8_t value, bool acked) {
    if (token_count < TOKEN_MAX) {
        tokens[token_count].kind = kind;
        tokens[token_count].value = value;
        tokens[token_count].acked = acked;
        token_count++;
    }
}

static bool
sda_effective_low(void) {
    return master_sda_low || slave_sda_low;
}

static bool
scl_effective_low(void) {
    return master_scl_low;
}

/* Sets up the slave's side of the wire during the SCL-low period, before
 * the next rising edge latches it. Runs on every falling edge; which of the
 * three things it does depends only on how many bits of this byte are
 * already latched. */
static void
slave_prepare_next_bit(void) {
    if (!decoding) {
        slave_sda_low = false;
        return;
    }

    if (decode_bit_index == 0) {
        current_byte_slave_driven = next_byte_slave_driven;
    }

    if (decode_bit_index < 8) {
        if (current_byte_slave_driven) {
            bool bit_is_one = (read_value & (1u << (7 - decode_bit_index))) != 0;
            slave_sda_low = !bit_is_one;
        } else {
            slave_sda_low = false;
        }
        return;
    }

    /* The ack slot. A byte the slave itself sent is acked or not by the
     * master, who never touches this model's drive; a byte the master sent
     * is acked here unless this is the byte position under test. */
    if (current_byte_slave_driven) {
        slave_sda_low = false;
    } else {
        slave_sda_low = master_byte_index != nack_at;
        master_byte_index++;
    }
}

static void
slave_on_8_bits_clocked(void) {
    if (current_byte_slave_driven) {
        /* This protocol only ever has the slave send one byte per read;
         * whatever comes after it - an ack slot the master decides, then a
         * repeated start or a stop - is not another slave-sent byte. */
        next_byte_slave_driven = false;
        return;
    }
    if (expect_address) {
        bool read_direction = (decode_byte_bits & 1u) != 0;
        next_byte_slave_driven = read_direction;
        expect_address = false;
    } else {
        next_byte_slave_driven = false;
    }
}

static void
commit_bit(bool bit_is_one) {
    if (decode_bit_index < 8) {
        decode_byte_bits = (uint8_t)((decode_byte_bits << 1) | (bit_is_one ? 1 : 0));
        decode_bit_index++;
        if (decode_bit_index == 8) {
            slave_on_8_bits_clocked();
        }
    } else {
        append_token(PMIC_BUS_TOKEN_BYTE, decode_byte_bits, !bit_is_one);
        decode_bit_index = 0;
        decode_byte_bits = 0;
    }
}

/* A rising edge only records what SDA reads; the falling edge commits it as
 * a bit. A repeated start is SCL released, then SDA driven low while still
 * high, so without this deferral that release would misread as a bit. */
static void
handle_scl_change(bool new_low) {
    bool old_low = master_scl_low;
    master_scl_low = new_low;
    if (old_low == new_low) {
        return;
    }

    if (old_low && !new_low) {
        if (decoding) {
            pending_sample_valid = true;
            pending_bit_is_one = !sda_effective_low();
        }
        return;
    }

    if (decoding && pending_sample_valid) {
        commit_bit(pending_bit_is_one);
    }
    pending_sample_valid = false;
    slave_prepare_next_bit();
}

static void
handle_sda_change(bool new_low) {
    bool old_master_low = master_sda_low;
    master_sda_low = new_low;
    if (old_master_low == new_low || master_scl_low) {
        return;
    }

    bool old_effective_low = old_master_low || slave_sda_low;
    bool new_effective_low = new_low || slave_sda_low;
    if (old_effective_low == new_effective_low) {
        return;
    }

    if (decoding && decode_bit_index != 0) {
        saw_mid_byte_edge = true;
    }
    pending_sample_valid = false;

    if (new_effective_low) {
        decoding = true;
        decode_bit_index = 0;
        decode_byte_bits = 0;
        expect_address = true;
        current_byte_slave_driven = false;
        next_byte_slave_driven = false;
        append_token(PMIC_BUS_TOKEN_START, 0, false);
    } else {
        decoding = false;
        append_token(PMIC_BUS_TOKEN_STOP, 0, false);
    }
}

void
gpio_ll_output_enable(gpio_dev_t* hw, uint32_t gpio_num) {
    (void)hw;
    gpio_calls++;
    if (gpio_num == PMIC_BUS_SDA_PIN) {
        handle_sda_change(true);
    } else if (gpio_num == PMIC_BUS_SCL_PIN) {
        handle_scl_change(true);
    }
}

void
gpio_ll_output_disable(gpio_dev_t* hw, uint32_t gpio_num) {
    (void)hw;
    gpio_calls++;
    if (gpio_num == PMIC_BUS_SDA_PIN) {
        handle_sda_change(false);
    } else if (gpio_num == PMIC_BUS_SCL_PIN) {
        handle_scl_change(false);
    }
}

void
gpio_ll_input_enable(gpio_dev_t* hw, uint32_t gpio_num) {
    (void)hw;
    (void)gpio_num;
    gpio_calls++;
}

void
gpio_ll_set_level(gpio_dev_t* hw, uint32_t gpio_num, uint32_t level) {
    (void)hw;
    (void)gpio_num;
    (void)level;
    gpio_calls++;
}

int
gpio_ll_get_level(gpio_dev_t* hw, uint32_t gpio_num) {
    (void)hw;
    gpio_calls++;
    if (gpio_num == PMIC_BUS_SDA_PIN) {
        return sda_effective_low() ? 0 : 1;
    }
    return scl_effective_low() ? 0 : 1;
}

void
gpio_ll_matrix_out_default(gpio_dev_t* hw, uint32_t gpio_num) {
    (void)hw;
    (void)gpio_num;
    gpio_calls++;
}

void
esp_rom_gpio_pad_select_gpio(uint32_t iopad_num) {
    (void)iopad_num;
    gpio_calls++;
}

void
esp_rom_delay_us(uint32_t us) {
    for (int i = 0; i < distinct_delay_count; i++) {
        if (distinct_delays[i] == us) {
            return;
        }
    }
    if (distinct_delay_count < DISTINCT_DELAYS_MAX) {
        distinct_delays[distinct_delay_count++] = us;
    }
}

soc_reset_reason_t
esp_rom_get_reset_reason(int cpu_no) {
    (void)cpu_no;
    return (soc_reset_reason_t)reset_reason;
}
