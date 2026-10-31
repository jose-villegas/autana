/* Development-only tone check on the console task. */
#include "sound/console/console_tone.h"

#include <math.h>
#include <stdio.h>

#include "audio/audio_codec_feedback.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "audio/audio_out_device.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_log.h"
#include "input/motion/imu.h"
#include "input/motion/imu_vibration.h"
#include "sound/console/console_tone_parse.h"
#include "sound/console/console_tone_reply.h"
#include "sound/sound_engine.h"

bool
console_tone_stop(sound_voice_t* voice) {
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    while (sound_is_playing(*voice) && timing_now_us() < deadline) {
        sound_stop(*voice);
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
    const bool stopped = !sound_is_playing(*voice);
    *voice = SOUND_VOICE_NULL;
    return stopped;
}

bool
console_tone_start(sound_voice_t* voice, uint16_t hz, uint16_t ms, uint8_t gain) {
    *voice = sound_play_tone(hz, ms, gain, SOUND_BUS_SYSTEM);
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    while (sound_is_playing(*voice) && !sound_voice_audible(*voice) && timing_now_us() < deadline) {
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
    if (!sound_voice_audible(*voice)) {
        console_tone_stop(voice);
        return false;
    }
    return true;
}

static const char* TAG = "tone";
static const char* TONE_SHAKE_READ_FAILED = "IMU read failed";
static const char* TONE_SHAKE_SHORT = "too few samples; other reader or short window";
static const char* TONE_SHAKE_PULSE_SHORT = "too few samples";
static const char* TONE_SHAKE_PULSE_GAP = "gap inside pulse";
static const char* TONE_SHAKE_UNAVAILABLE = "IMU unavailable";

typedef struct {
    imu_sample_t samples[IMU_FIFO_SAMPLES];
    uint8_t lost_before[IMU_FIFO_SAMPLES];
    int64_t sample_us[IMU_FIFO_SAMPLES];
    unsigned gaps;
    const char* reason;
} tone_shake_buffer_t;

/* Takes the queued samples; with `shake` NULL they are dropped to open a window.
 * A window long enough fills the queue, and its gaps are the overflow of that. Another reader
 * taking during the window leaves at most one drain, so anything short of full is n/a, logged. */
static bool
tone_shake_read(tone_shake_buffer_t* fifo, imu_vibration_t* shake) {
    bool gap = false;
    const int count = imu_take_fifo_at(IMU_BUZZ_ODR_CODE, fifo->samples, fifo->lost_before, &gap);
    if (count < 0) {
        fifo->reason = TONE_SHAKE_READ_FAILED;
        return false;
    }
    if (!shake) {
        return true;
    }
    fifo->gaps += gap;
    if (count < IMU_FIFO_SAMPLES) {
        fifo->reason = TONE_SHAKE_SHORT;
        return false;
    }
    return imu_vibration_measure(fifo->samples, count, shake);
}

/* audio_loopback_measure() plus the board's vibration over the newest IMU_FIFO_SAMPLES the IMU
 * task queued in the same window, read at IMU_BUZZ_ODR_CODE; `shake->samples` stays zero without one. */
static bool
tone_measure(const console_tone_args_t* args, float* db, audio_loopback_result_t* result, tone_shake_buffer_t* fifo,
             imu_vibration_t* shake) {
    const bool opened = fifo && tone_shake_read(fifo, NULL);
    const bool ok = audio_loopback_measure(args->hz, args->ms, db, result);
    if (!(opened && ok && tone_shake_read(fifo, shake))) {
        *shake = (imu_vibration_t){0};
    }
    return ok;
}

#define TONE_PULSE_SAMPLE_CAPACITY                                                                                     \
    ((CONSOLE_TONE_PULSE_WINDOW_MAX_MS * MATH_US_PER_MS / IMU_BUZZ_SAMPLE_US) + IMU_FIFO_SAMPLES)

typedef struct {
    tone_shake_buffer_t* fifo;
    imu_sample_t* samples;
    size_t count;
    int64_t start_us;
    uint16_t pulse_ms;
    bool gap;
    int64_t last_sample_us;
} tone_pulse_buffer_t;

static bool
tone_pulse_collect_sample(tone_pulse_buffer_t* pulse, int index) {
    const int64_t pulse_us = (int64_t)pulse->pulse_ms * MATH_US_PER_MS;
    const int64_t sample_us = pulse->fifo->sample_us[index] - pulse->start_us;
    const int64_t lost_us = (int64_t)pulse->fifo->lost_before[index] * IMU_BUZZ_SAMPLE_US;
    if (lost_us && sample_us - IMU_BUZZ_SAMPLE_US >= 0 && sample_us - lost_us <= pulse_us) {
        pulse->gap = true;
    }
    if (sample_us < 0) {
        return true;
    }
    if (sample_us > pulse_us) {
        return true;
    }
    if (pulse->count == TONE_PULSE_SAMPLE_CAPACITY) {
        return false;
    }
    pulse->samples[pulse->count++] = pulse->fifo->samples[index];
    return true;
}

/* Delivery may trail sampling by a task period; selection uses the producer clock. */
static bool
tone_pulse_drain(void* context) {
    tone_pulse_buffer_t* pulse = context;
    bool gap = false;
    const int count = imu_take_fifo_timed_at(IMU_BUZZ_ODR_CODE, pulse->fifo->samples, pulse->fifo->lost_before, &gap,
                                             pulse->fifo->sample_us);
    if (count < 0) {
        pulse->fifo->reason = TONE_SHAKE_READ_FAILED;
        return false;
    }
    pulse->fifo->gaps += gap;
    const int64_t end_us =
        count ? pulse->fifo->sample_us[count - 1] - pulse->start_us : timing_now_us() - pulse->start_us;
    const int64_t pulse_us = (int64_t)pulse->pulse_ms * MATH_US_PER_MS;
    const int64_t first_us = count ? pulse->fifo->sample_us[0] - pulse->start_us : end_us;
    if (gap && first_us >= 0 && pulse->last_sample_us < pulse_us) {
        pulse->gap = true;
    }
    if (count) {
        pulse->last_sample_us = end_us;
    }
    for (int i = 0; i < count; i++) {
        if (!tone_pulse_collect_sample(pulse, i)) {
            return false;
        }
    }
    return true;
}

static bool
tone_pulse_measure(const console_tone_args_t* args, float* db, audio_loopback_result_t* result,
                   tone_pulse_buffer_t* pulse, imu_vibration_t* shake) {
    if (!pulse) {
        return audio_loopback_measure_observed(args->hz, args->ms, db, result, NULL, NULL);
    }
    pulse->count = 0;
    pulse->gap = false;
    pulse->last_sample_us = 0;
    pulse->pulse_ms = args->pulse_ms;
    pulse->start_us = timing_now_us();
    bool ok = audio_loopback_measure_observed(args->hz, args->ms, db, result, tone_pulse_drain, pulse);
    const int64_t deadline = pulse->start_us + ((int64_t)(args->pulse_ms + IMU_BUZZ_WINDOW_MS) * MATH_US_PER_MS);
    while (ok && pulse->last_sample_us < (int64_t)args->pulse_ms * MATH_US_PER_MS && timing_now_us() < deadline) {
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
        ok = tone_pulse_drain(pulse);
    }
    if (pulse->gap) {
        pulse->fifo->reason = TONE_SHAKE_PULSE_GAP;
    } else if (pulse->count < IMU_VIBRATION_MIN_SAMPLES) {
        pulse->fifo->reason = TONE_SHAKE_PULSE_SHORT;
    }
    if (!ok || pulse->gap || !imu_vibration_measure(pulse->samples, (int)pulse->count, shake)) {
        *shake = (imu_vibration_t){0};
    }
    return ok;
}

static bool
tone_capture_window(const console_tone_args_t* args, float* db, audio_loopback_result_t* result,
                    tone_shake_buffer_t* fifo, tone_pulse_buffer_t* pulse, imu_vibration_t* shake) {
    const bool ok = args->pulse_ms ? tone_pulse_measure(args, db, result, pulse, shake)
                                   : tone_measure(args, db, result, fifo, shake);
    if (!shake->samples) {
        ESP_LOGW(TAG, "shake n/a: %s", fifo && fifo->reason ? fifo->reason : TONE_SHAKE_UNAVAILABLE);
    }
    return ok;
}

static tone_pulse_buffer_t*
tone_pulse_open(const console_tone_args_t* args, tone_shake_buffer_t* fifo) {
    if (!args->pulse_ms || !fifo) {
        return NULL;
    }
    tone_pulse_buffer_t* pulse =
        memory_alloc(sizeof *pulse + (TONE_PULSE_SAMPLE_CAPACITY * sizeof(imu_sample_t)), MEMORY_PSRAM);
    if (pulse) {
        pulse->samples = (imu_sample_t*)(pulse + 1);
        pulse->fifo = fifo;
    }
    return pulse;
}

/* The IMU at IMU_BUZZ_ODR_CODE and a buffer for its samples; NULL leaves both as they were. */
static tone_shake_buffer_t*
tone_shake_open(void) {
    tone_shake_buffer_t* fifo = imu_ready() ? memory_alloc(sizeof *fifo, MEMORY_PSRAM) : NULL;
    if (fifo && !imu_set_odr(IMU_BUZZ_ODR_CODE)) {
        memory_free(fifo);
        fifo = NULL;
    }
    if (fifo) {
        fifo->gaps = 0;
        fifo->reason = NULL;
        (void)tone_shake_read(fifo, NULL);
    }
    return fifo;
}

/* False when the IMU could not return to IMU_ODR_CODE. */
static bool
tone_shake_close(tone_shake_buffer_t* fifo) {
    if (!fifo) {
        return true;
    }
    memory_free(fifo);
    return imu_set_odr(IMU_ODR_CODE);
}

static bool
tone_capture_close(tone_shake_buffer_t* fifo, bool held) {
    bool ok = tone_shake_close(fifo);
    if (held) {
        timing_sleep_ms(AUDIO_OUT_DMA_DRAIN_MS);
    }
    ok = audio_codec_dac_feedback(false) && ok;
    ok = audio_out_amp_hold(false) && ok;
    return sound_capture_close() && ok;
}

const char*
console_tone_check(const console_tone_args_t* args, char result[CONSOLE_TONE_REPLY_BYTES]) {
    const size_t dma_before = memory_free_bytes(MEMORY_DMA);
    const bool opened = sound_capture_open();
    const size_t dma_open = dma_before - memory_free_bytes(MEMORY_DMA);
    float silence_db = 0;
    float tone_db = 0;
    audio_loopback_result_t loopback = {0};
    audio_loopback_result_t silence = {0};
    sound_voice_t voice = SOUND_VOICE_NULL;
    tone_shake_buffer_t* fifo = tone_shake_open();
    imu_vibration_t silence_shake = {0};
    imu_vibration_t tone_shake = {0};
    tone_pulse_buffer_t* pulse = tone_pulse_open(args, fifo);
    bool held = false;
    bool ok = opened && audio_codec_dac_feedback(false)
              && (!args->internal || ((held = audio_out_amp_hold(true)) && audio_codec_dac_feedback(true)))
              && (!args->pulse_ms || !fifo || pulse)
              && tone_capture_window(args, &silence_db, &silence, fifo, pulse, &silence_shake)
              && console_tone_start(&voice, args->hz, args->pulse_ms, args->volume_pct)
              && tone_capture_window(args, &tone_db, &loopback, fifo, pulse, &tone_shake);
    memory_free(pulse);
    ok = console_tone_stop(&voice) && ok;
    const unsigned imu_gaps = fifo ? fifo->gaps : 0;
    const char* shake_reason = fifo ? fifo->reason : TONE_SHAKE_UNAVAILABLE;
    ok = tone_capture_close(fifo, held) && ok;
    loopback.noise_dbfs = silence.noise_dbfs;
    const bool heard = tone_db - silence_db >= AUDIO_LOOPBACK_HEARD_MARGIN_DB;
    if (!ok) {
        return opened ? "loopback failed" : "codec open failed";
    }
    if (!console_tone_reply(result, CONSOLE_TONE_REPLY_BYTES, args, heard, tone_db, silence_db, &loopback, dma_open)
        || !console_tone_append_shake(result, CONSOLE_TONE_REPLY_BYTES, &silence_shake, &tone_shake, imu_gaps,
                                      silence_shake.samples && tone_shake.samples ? NULL : shake_reason)) {
        return "frequency formatting failed";
    }
    return NULL;
}

static void
console_verb_tone(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    if (!console_tone_parse(args, &parsed)) {
        reply("TONE usage: tone <hz> <ms> [volume] [pulse <ms>] [internal]");
        return;
    }
    char line[CONSOLE_TONE_REPLY_BYTES];
    const char* error = console_tone_check(&parsed, line);
    if (error) {
        (void)snprintf(line, sizeof line, "TONE error: %s", error);
        reply(line);
        return;
    }
    (void)printf("%s\n", line);
    (void)fflush(stdout);
}
CONSOLE_VERB(tone, 0, console_verb_tone)
