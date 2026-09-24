#pragma once
#include <stdio.h>

/* pmic_cold_boot.c only ever logs at these two levels. */
#define ESP_LOGI(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)
#define ESP_LOGW(tag, ...)                                                                                             \
    do {                                                                                                               \
        (void)(tag);                                                                                                   \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)
