/* Optional per-translation-unit call counts; begin/report require quiescent frame workers. */
#pragma once

#ifdef __XTENSA__
#include "sdkconfig.h"
#endif

#if defined(CONFIG_MATH_COUNT_CALLS) && CONFIG_MATH_COUNT_CALLS
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct math_call_record {
    const char* function;
    const char* caller;
    atomic_uint count;
    atomic_bool registered;
    struct math_call_record* next;
} math_call_record_t;

void math_calls_hit(math_call_record_t* record);
void math_calls_begin(void);
void math_calls_report(uint32_t frames);

#define MATH_FAST_CALL(fn)                                                                                             \
    do {                                                                                                               \
        static math_call_record_t math_call_record = {.function = #fn, .caller = __BASE_FILE__};                       \
        math_calls_hit(&math_call_record);                                                                             \
    } while (0)
#else
#define MATH_FAST_CALL(fn)        ((void)0)
#define math_calls_begin()        ((void)0)
#define math_calls_report(frames) ((void)0)
#endif
