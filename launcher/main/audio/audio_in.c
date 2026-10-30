/* Microphone device over the shared codec and I2S lifetime. */
#include "audio/audio_in.h"

#include <limits.h>

#include "audio/audio_codec_device.h"
#include "audio/audio_codec_mic.h"
#include "core/tune.h"

/* At 24 dB, a 1 kHz tone clipped the mic above 10% volume; 6 dB keeps full volume below full scale. */
#define AUDIO_IN_GAIN_DB 6.0F

TUNE_OWNER(mic);
#define MIC_TUNE_NIBBLE(name) TUNE(mic, name, AUDIO_CODEC_MIC_FIELD_DEFAULT, 0, AUDIO_CODEC_MIC_NIBBLE_MAX)
TUNE(mic, eq, AUDIO_CODEC_MIC_OFF_DEFAULT, 0, 1);
TUNE(mic, alc, AUDIO_CODEC_MIC_OFF_DEFAULT, 0, 1);
MIC_TUNE_NIBBLE(alc_max);
MIC_TUNE_NIBBLE(alc_min);
MIC_TUNE_NIBBLE(alc_win);
TUNE(mic, alc_gain, AUDIO_CODEC_MIC_VOLUME_DEFAULT, 0, AUDIO_CODEC_MIC_GAIN_MAX);
TUNE(mic, gate, AUDIO_CODEC_MIC_OFF_DEFAULT, 0, 1);
MIC_TUNE_NIBBLE(gate_ng);
MIC_TUNE_NIBBLE(gate_win);
TUNE(mic, gate_depth, AUDIO_CODEC_MIC_FIELD_DEFAULT, 0, AUDIO_CODEC_MIC_DEPTH_MAX);
TUNE(mic, hpf, AUDIO_CODEC_MIC_DYNAMIC_DEFAULT, 0, 1);
TUNE(mic, hpf_s1, AUDIO_CODEC_MIC_HPF_DEFAULT, 0, AUDIO_CODEC_MIC_HPF_MAX);
TUNE(mic, hpf_s2, AUDIO_CODEC_MIC_HPF_DEFAULT, 0, AUDIO_CODEC_MIC_HPF_MAX);
static esp_codec_dev_handle_t input;
static bool close_failed;

static bool
apply(void) {
    const audio_codec_mic_t config = {.eq = (uint8_t)eq,
                                      .alc = (uint8_t)alc,
                                      .alc_max = (uint8_t)alc_max,
                                      .alc_min = (uint8_t)alc_min,
                                      .alc_win = (uint8_t)alc_win,
                                      .alc_gain = (uint8_t)alc_gain,
                                      .gate = (uint8_t)gate,
                                      .gate_ng = (uint8_t)gate_ng,
                                      .gate_win = (uint8_t)gate_win,
                                      .gate_depth = (uint8_t)gate_depth,
                                      .hpf = (uint8_t)hpf,
                                      .hpf_s1 = (uint8_t)hpf_s1,
                                      .hpf_s2 = (uint8_t)hpf_s2};
    return !(alc || gate || TUNE_ENABLED) || audio_codec_mic_set(&config);
}

bool
audio_in_open(void) {
    if (close_failed) {
        audio_in_close();
        if (close_failed) {
            return false;
        }
    }
    if (input) {
        return true;
    }
    input = audio_codec_device_open(ESP_CODEC_DEV_TYPE_IN);
    if (!input) {
        return false;
    }
    if (!audio_codec_check("microphone gain", esp_codec_dev_set_in_gain(input, AUDIO_IN_GAIN_DB)) || !apply()) {
        audio_in_close();
        return false;
    }
    return true;
}

void
audio_in_close(void) {
    close_failed = !audio_codec_mic_restore();
    if (close_failed) {
        return;
    }
    audio_codec_device_close(input);
    input = NULL;
}

bool
audio_in_read(int16_t* samples, size_t count) {
    audio_codec_assert_not_frame_task();
    if (!input || !samples || count > INT_MAX / sizeof *samples) {
        return false;
    }
    return audio_codec_check("read", esp_codec_dev_read(input, samples, (int)(count * sizeof *samples)));
}
