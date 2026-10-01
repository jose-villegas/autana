/* log_device: log.h over the ESP-IDF log, keeping its "I (ms) tag:" prefix. */

#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"

static void
emit(esp_log_level_t level, const char* tag, const char* format, va_list args) {
    char line[LOG_LINE_MAX];
    if (vsnprintf(line, sizeof line, format, args) < 0) {
        line[0] = '\0';
    }
    ESP_LOG_LEVEL(level, tag, "%s", line);
}

void
log_info(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    emit(ESP_LOG_INFO, tag, format, args);
    va_end(args);
}

void
log_warn(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    emit(ESP_LOG_WARN, tag, format, args);
    va_end(args);
}

void
log_error(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    emit(ESP_LOG_ERROR, tag, format, args);
    va_end(args);
}
