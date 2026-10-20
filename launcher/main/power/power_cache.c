#include "power/power_cache.h"

void
power_cache_store(power_cache_t* cache, const power_battery_t* battery) {
    cache->enter(cache->context);
    if (battery->read_us >= cache->battery.read_us) {
        cache->battery = *battery;
    }
    cache->leave(cache->context);
}

power_battery_t
power_cache_load(power_cache_t* cache) {
    cache->enter(cache->context);
    power_battery_t out = cache->battery;
    cache->leave(cache->context);
    return out;
}
