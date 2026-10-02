#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

/* A host test defines these to capture what log_device.c writes. */
void esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...);
void esp_log_writev(esp_log_level_t level, const char* tag, const char* format, va_list args);
uint32_t esp_log_timestamp(void);
/* Argument-checked, so a wrong format string still fails the check. */
#define ESP_LOGE(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
    } while (0)
#define ESP_LOGW(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
    } while (0)
#define ESP_LOGI(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
    } while (0)
#define ESP_LOGD(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
    } while (0)
#define ESP_LOGV(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
    } while (0)
