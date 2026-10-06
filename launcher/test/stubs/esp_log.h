#pragma once
#include <stdio.h>
/* Argument-checked, so a wrong format string still fails the check. To stderr
 * with a newline, as the board's log is a line apart from a program's output:
 * a host render writes its image to stdout. */
#define ESP_LOG_STUB(tag, ...)                                                                                         \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        (void)fprintf(stderr, __VA_ARGS__);                                                                            \
        (void)fputc('\n', stderr);                                                                                     \
    } while (0)
#define ESP_LOGE(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) ESP_LOG_STUB(tag, __VA_ARGS__)
