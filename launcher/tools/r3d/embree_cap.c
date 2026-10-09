/* Caps the instruction set of the Embree device Mitsuba creates. Mitsuba
 * creates it from a fixed configuration string, and Embree picks its kernels
 * by the host CPU, so the same rays return distances an ulp apart on an
 * AVX-512 host and an AVX2 one. Loaded with RTLD_GLOBAL before
 * Mitsuba, this definition of Mitsuba's namespaced rtcNewDevice comes first in
 * the lookup and appends `cap` to the configuration before calling Embree's
 * own, which r3d/isa.py binds by address. */

#include <stddef.h>
#include <stdio.h>

typedef void* (*new_device_t)(const char* config);

static new_device_t embree_new_device;
static char cap[64];
static char last_config[512];

void
embree_cap_bind(new_device_t new_device, const char* config_cap) {
    embree_new_device = new_device;
    snprintf(cap, sizeof cap, "%s", config_cap);
}

/* The configuration the last device was created with, empty before one was. */
const char*
embree_cap_last_config(void) {
    return last_config;
}

void*
_ZN7mitsuba12rtcNewDeviceEPKc(const char* config) {
    const char* given = config ? config : "";
    const int length = snprintf(last_config, sizeof last_config, "%s%s%s", given, *given ? "," : "", cap);
    /* A cut configuration would lose the cap: no device, and Mitsuba stops. */
    if (length < 0 || (size_t)length >= sizeof last_config) {
        last_config[0] = '\0';
        return NULL;
    }
    return embree_new_device(last_config);
}
