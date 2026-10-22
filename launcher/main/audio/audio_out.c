/* Blocking PCM output, used from a worker or console task. */
#include "audio/audio_out.h"

#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "audio/audio_codec_device.h"
#include "audio/audio_codec_drc.h"
#include "audio/audio_out_device.h"
#include "board/board.h"
#include "build/build_variant.h"
#include "services/tune.h"

#define AUDIO_OUT_CODEC_VOLUME        70
#define AUDIO_OUT_CODEC_VOLUME_MIN_DB (-50.0F)
#define AUDIO_OUT_CODEC_VOLUME_MAX_DB 0.0F
#define AUDIO_OUT_DRC_ON              1
#define AUDIO_OUT_DRC_MAX_LEVEL       2
#define AUDIO_OUT_DRC_MIN_LEVEL       2
#define AUDIO_OUT_DRC_WINSIZE         4
#define AUDIO_OUT_DRC_RAMP            2
TUNE_OWNER(audio);
TUNE(audio, codec_volume, AUDIO_OUT_CODEC_VOLUME, 0, AUDIO_OUT_VOLUME_MAX_PCT);
TUNE(audio, drc, AUDIO_OUT_DRC_ON, 0, 1);
TUNE(audio, drc_max, AUDIO_OUT_DRC_MAX_LEVEL, 0, AUDIO_CODEC_DRC_FIELD_MAX);
TUNE(audio, drc_min, AUDIO_OUT_DRC_MIN_LEVEL, 0, AUDIO_CODEC_DRC_FIELD_MAX);
TUNE(audio, drc_win, AUDIO_OUT_DRC_WINSIZE, 0, AUDIO_CODEC_DRC_FIELD_MAX);
TUNE(audio, drc_ramp, AUDIO_OUT_DRC_RAMP, 0, AUDIO_CODEC_DRC_FIELD_MAX);
static esp_codec_dev_handle_t output;
#if CONFIG_LAUNCHER_DEVELOPMENT
static uint32_t applied_generation;
#endif

static bool
apply(void) {
    const audio_codec_drc_t config = {.on = drc,
                                      .winsize = (uint8_t)drc_win,
                                      .max_level = (uint8_t)drc_max,
                                      .min_level = (uint8_t)drc_min,
                                      .ramp = (uint8_t)drc_ramp};
    return audio_codec_check("volume", esp_codec_dev_set_out_vol(output, codec_volume))
           && (!(drc || TUNE_ENABLED) || audio_codec_drc_set(&config));
}
#if CONFIG_LAUNCHER_DEVELOPMENT
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

#endif

bool
audio_out_amp_enable(bool enabled) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    return set_amp(false, enabled);
#else
    return audio_codec_check("amplifier", board_audio_amp_enable(enabled));
#endif
}

#if CONFIG_LAUNCHER_DEVELOPMENT
bool
audio_out_amp_hold(bool held) {
    return set_amp(true, held);
}

#endif

bool
audio_out_open(void) {
    if (output) {
        return true;
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (!amp_mutex) {
        amp_mutex = xSemaphoreCreateMutexStatic(&amp_mutex_storage);
    }
#endif
    output = audio_codec_device_open(ESP_CODEC_DEV_TYPE_OUT);
    if (!output) {
        return false;
    }
    esp_codec_dev_vol_map_t points[] = {
        {.vol = 0, .db_value = AUDIO_OUT_CODEC_VOLUME_MIN_DB},
        {.vol = AUDIO_OUT_VOLUME_MAX_PCT, .db_value = AUDIO_OUT_CODEC_VOLUME_MAX_DB},
    };
    esp_codec_dev_vol_curve_t curve = {.vol_map = points, .count = sizeof points / sizeof *points};
    if (!audio_codec_check("volume curve", esp_codec_dev_set_vol_curve(output, &curve)) || !apply()) {
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
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (applied_generation != TUNE_GENERATION(audio)) {
        const uint32_t generation = TUNE_GENERATION(audio);
        if (audio_codec_check("re-apply", apply() ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR)) {
            applied_generation = generation;
        }
    }
#endif
    return audio_codec_check("write", esp_codec_dev_write(output, (void*)samples, (int)(count * sizeof *samples)));
}

void
audio_out_close(void) {
    audio_codec_device_close(output);
    output = NULL;
}
