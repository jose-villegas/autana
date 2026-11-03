/* Stable console tone result formatting, including unavailable frequency estimates. */
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_loopback.h"
#include "input/motion/imu_vibration.h"
#include "sound/console/console_tone_parse.h"

#define CONSOLE_TONE_DB_BYTES      16u
#define CONSOLE_TONE_INTEGER_BYTES (sizeof(size_t) * 3u)
#define CONSOLE_TONE_REPLY_BYTES                                                                                       \
    (sizeof("TONE not heard  Hz:  dB (silence  dB), noise  dBFS, path acoustic, measured , open  B DMA, thd  dB, "     \
            "peak  dBFS, shake  mg rms  mg peak (silence  mg rms)")                                                    \
     + (9u * CONSOLE_TONE_DB_BYTES) + (2u * CONSOLE_TONE_INTEGER_BYTES) + CONSOLE_TONE_MEASURED_BYTES)
#define CONSOLE_TONE_MEASURED_BYTES 32u

static inline bool
console_tone_reply(char* line, size_t capacity, const console_tone_args_t* args, bool heard, float tone_db,
                   float silence_db, const audio_loopback_result_t* result, size_t dma_open) {
    char measured[CONSOLE_TONE_MEASURED_BYTES];
    const int measured_length = isfinite(result->measured_hz)
                                    ? snprintf(measured, sizeof measured, "%.2f Hz", (double)result->measured_hz)
                                    : snprintf(measured, sizeof measured, "n/a");
    if (measured_length < 0 || (size_t)measured_length >= sizeof measured) {
        return false;
    }
    char thd[CONSOLE_TONE_MEASURED_BYTES];
    const int thd_length = isfinite(result->thd_db) ? snprintf(thd, sizeof thd, "%.1f", (double)result->thd_db)
                                                    : snprintf(thd, sizeof thd, "n/a");
    if (thd_length < 0 || (size_t)thd_length >= sizeof thd) {
        return false;
    }
    const int length = snprintf(line, capacity,
                                "TONE %s %u Hz: %.1f dB (silence %.1f dB), noise %.1f dBFS, path %s, measured %s, open "
                                "%zu B DMA, thd %s dB, peak %.1f dBFS",
                                heard ? "heard" : "not heard", (unsigned)args->hz, (double)tone_db, (double)silence_db,
                                (double)result->noise_dbfs, args->internal ? "internal" : "acoustic", measured,
                                dma_open, thd, (double)result->peak_dbfs);
    return length >= 0 && (size_t)length < capacity;
}

/* Appends the board's vibration in both windows, or "n/a" when either has no IMU reading. */
static inline bool
console_tone_append_shake(char* line, size_t capacity, const imu_vibration_t* silence, const imu_vibration_t* tone) {
    const char* end = memchr(line, '\0', capacity);
    if (!end) {
        return false;
    }
    const size_t used = (size_t)(end - line);
    const int length =
        silence->samples && tone->samples
            ? snprintf(line + used, capacity - used, ", shake %.2f mg rms %.2f mg peak (silence %.2f mg rms)",
                       (double)tone->rms_mg, (double)tone->peak_mg, (double)silence->rms_mg)
            : snprintf(line + used, capacity - used, ", shake n/a");
    return length >= 0 && (size_t)length < capacity - used;
}
