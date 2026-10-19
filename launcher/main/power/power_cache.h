/* Lock hooks cover whole snapshots, never I2C. */
#pragma once
#include "power/power.h"

typedef void (*power_cache_lock_fn)(void* context);

typedef struct {
    power_battery_t battery;
    power_cache_lock_fn enter;
    power_cache_lock_fn leave;
    void* context;
} power_cache_t;

void power_cache_store(power_cache_t* cache, const power_battery_t* battery);
power_battery_t power_cache_load(power_cache_t* cache);
