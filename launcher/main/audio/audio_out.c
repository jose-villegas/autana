/* Blocking PCM output, used from a worker or console task. */
#include "audio/audio_out.h"

#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "audio/audio_codec.h"
#include "audio/audio_out_device.h"
#include "board/board.h"

#define AUDIO_OUT_CODEC_VOLUME 30
static esp_codec_dev_handle_t output;
static audio_out_amp_t amp;
static StaticSemaphore_t amp_mutex_storage;
static SemaphoreHandle_t amp_mutex;

static bool
set_amp(bool hold, bool enabled) {
    if (!amp_mutex) {
        return false;
    }
    /* A mutex serializes console holds and mixer requests, including the GPIO write. */
    xSemaphoreTake(amp_mutex, portMAX_DELAY);
    const int status = board_audio_amp_enable(audio_out_amp_update(&amp, hold, enabled));
    xSemaphoreGive(amp_mutex);
    return audio_codec_check("amplifier", status);
}

bool
audio_out_amp_enable(bool enabled) {
    return set_amp(false, enabled);
}

bool
audio_out_amp_hold(bool held) {
    return set_amp(true, held);
}

bool
audio_out_open(void) {
    if (output) {
        return true;
    }
    if (!amp_mutex) {
        amp_mutex = xSemaphoreCreateMutexStatic(&amp_mutex_storage);
    }
    output = audio_codec_device_open(ESP_CODEC_DEV_TYPE_OUT);
    if (!output) {
        return false;
    }
    if (!audio_codec_check("volume", esp_codec_dev_set_out_vol(output, AUDIO_OUT_CODEC_VOLUME))) {
        audio_out_close();
        return false;
    }
    return true;
}

bool
audio_out_write(const int16_t* samples, size_t count) {
    audio_codec_assert_not_frame_task();
    if (!output || !samples || count > INT_MAX / sizeof *samples) {
        return false;
    }
    return audio_codec_check("write", esp_codec_dev_write(output, (void*)samples, (int)(count * sizeof *samples)));
}

void
audio_out_close(void) {
    audio_codec_device_close(output);
    output = NULL;
}
