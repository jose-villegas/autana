/* log_device: log.h over the ESP-IDF log, keeping its "I (ms) tag:" prefix. */

#include "util/log.h"

#include <inttypes.h>
#include <stdarg.h>

#include "esp_log.h"

/* The prefix, the caller's text and the newline are written one after the
 * other, so no line length is imposed and none is cut. */
static void
emit(esp_log_level_t level, char letter, const char* tag, const char* format, va_list args) {
    esp_log_write(level, tag, "%c (%" PRIu32 ") %s: ", letter, esp_log_timestamp(), tag);
    esp_log_writev(level, tag, format, args);
    esp_log_write(level, tag, "\n");
}

void
log_info(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    emit(ESP_LOG_INFO, 'I', tag, format, args);
    va_end(args);
}

void
log_warn(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    emit(ESP_LOG_WARN, 'W', tag, format, args);
    va_end(args);
}

void
log_error(const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    emit(ESP_LOG_ERROR, 'E', tag, format, args);
    va_end(args);
}
