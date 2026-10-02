/*
 * log: lines for the console, tagged and levelled, so a caller above the
 * drivers reports without naming the logging library. Macros that forward
 * to the platform logger, not functions: a function taking the caller's
 * arguments would need the logger's variadic-list entry points, which cost
 * internal RAM the firmware does not otherwise carry, and a line is written
 * whole however long it is. A device half beside a driver may call the
 * logging library directly.
 */
#pragma once

#include "esp_log.h"

#define log_info(tag, ...)  ESP_LOGI(tag, __VA_ARGS__)
#define log_warn(tag, ...)  ESP_LOGW(tag, __VA_ARGS__)
#define log_error(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
