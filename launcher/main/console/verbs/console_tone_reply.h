/* Stable console tone result formatting, including unavailable frequency estimates. */
#pragma once
#include <math.h>
#include <stdio.h>

#include "console/verbs/console_tone_parse.h"

#define CONSOLE_TONE_REPLY_BYTES    192u
#define CONSOLE_TONE_MEASURED_BYTES 32u

static inline bool
console_tone_reply(char* line, size_t capacity, const console_tone_args_t* args, bool heard, float tone_db,
                   float silence_db, float measured_hz, size_t dma_open) {
    char measured[CONSOLE_TONE_MEASURED_BYTES];
    const int measured_length = isfinite(measured_hz)
                                    ? snprintf(measured, sizeof measured, "%.2f Hz", (double)measured_hz)
                                    : snprintf(measured, sizeof measured, "n/a");
    if (measured_length < 0 || (size_t)measured_length >= sizeof measured) {
        return false;
    }
    const int length =
        snprintf(line, capacity, "TONE %s %u Hz: %.1f dB (silence %.1f dB), path %s, measured %s, open %zu B DMA",
                 heard ? "heard" : "not heard", (unsigned)args->hz, (double)tone_db, (double)silence_db,
                 args->internal ? "internal" : "acoustic", measured, dma_open);
    return length >= 0 && (size_t)length < capacity;
}
