/*
 * settings: small integers that survive a reboot, in flash. A key lives in a
 * named space, so two owners never share a name. Defined in settings_device.c,
 * device only.
 *
 * The store starts on first use, and a store that is full or written by a
 * newer format is erased and started again: a setting is a preference, never
 * data worth refusing to boot over.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* True with `*out` set when the key holds a value; false when it was never
 * written or the store is unavailable. */
bool settings_read_i32(const char* space, const char* key, int32_t* out);

/* True when the value is stored and committed. */
bool settings_write_i32(const char* space, const char* key, int32_t value);
