/* Stable console tone result formatting, including unavailable frequency estimates. */
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio_loopback.h"
#include "collections/strbuf.h"
#include "input/motion/imu_vibration.h"
#include "sound/console/console_tone_parse.h"

#define CONSOLE_TONE_DB_BYTES      16u
#define CONSOLE_TONE_INTEGER_BYTES (sizeof(size_t) * 3u)
#define CONSOLE_TONE_REPLY_BYTES                                                                                       \
    (sizeof("TONE not heard  Hz:  dB (silence  dB), noise  dBFS, path acoustic, measured , open  B DMA, thd  dB, "     \
            "peak  dBFS, pulse_ms , shake  mg rms  mg peak (silence  mg rms), gx_mg , gy_mg , gz_mg , "                \
            "imu_gaps , shake_reason too few samples; other reader or short window")                                   \
     + (2u * CONSOLE_TONE_PULSE_REPLY_BYTES) + (13u * CONSOLE_TONE_DB_BYTES) + (4u * CONSOLE_TONE_INTEGER_BYTES)       \
     + CONSOLE_TONE_MEASURED_BYTES)
#define CONSOLE_TONE_MEASURED_BYTES 32u

#define CONSOLE_TONE_PULSE_REPLY_BYTES                                                                                 \
    (sizeof(", silence_pulse unavailable, silence_take_gaps , silence_take_ms , silence_pulse_gaps , "                 \
            "silence_pulse_gap_ms , silence_lost_samples , silence_lost_ms ")                                          \
     + (3u * CONSOLE_TONE_INTEGER_BYTES) + (3u * CONSOLE_TONE_DB_BYTES))

typedef struct {
    const char* status;
    unsigned take_gaps;
    unsigned pulse_gaps;
    unsigned lost_samples;
    int64_t take_us;
    int64_t lost_us;
    int64_t pulse_gap_us;
} console_tone_pulse_result_t;

static inline bool
console_tone_append_pulse(char* line, size_t capacity, const char* window, const console_tone_pulse_result_t* pulse) {
    const char* end = memchr(line, '\0', capacity);
    if (!end) {
        return false;
    }
    strbuf_t buffer = strbuf_at(line, capacity, (size_t)(end - line));
    return strbuf_appendf(&buffer,
                          ", %s_pulse %s, %s_take_gaps %u, %s_take_ms %.3f, %s_pulse_gaps %u, %s_pulse_gap_ms "
                          "%.3f, %s_lost_samples %u, %s_lost_ms %.3f",
                          window, pulse->status ? pulse->status : "unavailable", window, pulse->take_gaps, window,
                          (double)pulse->take_us / MATH_US_PER_MS, window, pulse->pulse_gaps, window,
                          (double)pulse->pulse_gap_us / MATH_US_PER_MS, window, pulse->lost_samples, window,
                          (double)pulse->lost_us / MATH_US_PER_MS);
}

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
    const int length =
        snprintf(line, capacity,
                 "TONE %s %u Hz: %.1f dB (silence %.1f dB), noise %.1f dBFS, path %s, measured %s, open %zu B DMA, thd "
                 "%s dB, peak %.1f dBFS, pulse_ms %u",
                 heard ? "heard" : "not heard", (unsigned)args->hz, (double)tone_db, (double)silence_db,
                 (double)result->noise_dbfs, args->internal ? "internal" : "acoustic", measured, dma_open, thd,
                 (double)result->peak_dbfs, (unsigned)args->pulse_ms);
    return length >= 0 && (size_t)length < capacity;
}

/* Appends the board's vibration in both windows, or "n/a" when either has no IMU reading. */
static inline bool
console_tone_append_shake(char* line, size_t capacity, const imu_vibration_t* silence, const imu_vibration_t* tone,
                          unsigned gaps, const char* reason) {
    const char* end = memchr(line, '\0', capacity);
    if (!end) {
        return false;
    }
    size_t used = (size_t)(end - line);
    const int length =
        silence->samples && tone->samples
            ? snprintf(line + used, capacity - used, ", shake %.2f mg rms %.2f mg peak (silence %.2f mg rms)",
                       (double)tone->rms_mg, (double)tone->peak_mg, (double)silence->rms_mg)
            : snprintf(line + used, capacity - used, ", shake n/a");
    if (length < 0 || (size_t)length >= capacity - used) {
        return false;
    }
    used += (size_t)length;
    if (silence->samples) {
        const int gravity_length = snprintf(line + used, capacity - used, ", gx_mg %.2f, gy_mg %.2f, gz_mg %.2f",
                                            (double)silence->gx_mg, (double)silence->gy_mg, (double)silence->gz_mg);
        if (gravity_length < 0 || (size_t)gravity_length >= capacity - used) {
            return false;
        }
        used += (size_t)gravity_length;
    }
    const int status_length = snprintf(line + used, capacity - used, ", imu_gaps %u%s%s", gaps,
                                       reason ? ", shake_reason " : "", reason ? reason : "");
    return status_length >= 0 && (size_t)status_length < capacity - used;
}
