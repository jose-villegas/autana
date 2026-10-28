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
console_tone_start(sound_voice_t* voice, uint16_t hz, uint8_t gain) {
    *voice = sound_play_tone(hz, SOUND_TONE_UNTIL_STOPPED, gain, SOUND_BUS_SYSTEM);
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

typedef struct {
    imu_sample_t samples[IMU_FIFO_SAMPLES];
    uint8_t lost_before[IMU_FIFO_SAMPLES];
} tone_shake_buffer_t;

/* Takes the queued samples; with `shake` NULL they are dropped to open a window.
 * A window long enough fills the queue, and its gaps are the overflow of that. Another reader
 * taking during the window leaves at most one drain, so anything short of full is n/a, logged. */
static bool
tone_shake_read(tone_shake_buffer_t* fifo, imu_vibration_t* shake) {
    bool gap = false;
    const int count = imu_take_fifo_at(IMU_BUZZ_ODR_CODE, fifo->samples, fifo->lost_before, &gap);
    if (count < 0 || !shake) {
        return count >= 0;
    }
    if (count < IMU_FIFO_SAMPLES) {
        ESP_LOGW(TAG, "shake n/a: %d of %d IMU samples; another reader took them or the window is short", count,
                 IMU_FIFO_SAMPLES);
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

/* The IMU at IMU_BUZZ_ODR_CODE and a buffer for its samples; NULL leaves both as they were. */
static tone_shake_buffer_t*
tone_shake_open(void) {
    tone_shake_buffer_t* fifo = imu_ready() ? memory_alloc(sizeof *fifo, MEMORY_PSRAM) : NULL;
    if (fifo && !imu_set_odr(IMU_BUZZ_ODR_CODE)) {
        memory_free(fifo);
        fifo = NULL;
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
    bool held = false;
    bool ok = opened && audio_codec_dac_feedback(false)
              && (!args->internal || ((held = audio_out_amp_hold(true)) && audio_codec_dac_feedback(true)))
              && tone_measure(args, &silence_db, &silence, fifo, &silence_shake)
              && console_tone_start(&voice, args->hz, args->volume_pct)
              && tone_measure(args, &tone_db, &loopback, fifo, &tone_shake);
    ok = console_tone_stop(&voice) && ok;
    ok = tone_shake_close(fifo) && ok;
    if (held) {
        timing_sleep_ms(AUDIO_OUT_DMA_DRAIN_MS);
    }
    loopback.noise_dbfs = silence.noise_dbfs;
    const bool heard = tone_db - silence_db >= AUDIO_LOOPBACK_HEARD_MARGIN_DB;
    ok = audio_codec_dac_feedback(false) && ok;
    ok = audio_out_amp_hold(false) && ok;
    ok = sound_capture_close() && ok;
    if (!ok) {
        return opened ? "loopback failed" : "codec open failed";
    }
    if (!console_tone_reply(result, CONSOLE_TONE_REPLY_BYTES, args, heard, tone_db, silence_db, &loopback, dma_open)
        || !console_tone_append_shake(result, CONSOLE_TONE_REPLY_BYTES, &silence_shake, &tone_shake)) {
        return "frequency formatting failed";
    }
    return NULL;
}

static void
console_verb_tone(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    if (!console_tone_parse(args, &parsed)) {
        reply("TONE usage: tone <hz> <ms> [volume] [internal]");
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
