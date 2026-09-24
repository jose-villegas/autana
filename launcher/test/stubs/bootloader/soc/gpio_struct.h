#pragma once

/* pmic_cold_boot.c only ever passes &GPIO through as an opaque handle to the
 * gpio_ll_* calls below; it never reads a field, so the real register
 * layout is not needed here. */
typedef struct {
    int unused;
} gpio_dev_t;

extern gpio_dev_t GPIO;
