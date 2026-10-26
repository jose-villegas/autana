/* Microphone device over the shared codec and I2S lifetime. */
#include "audio/audio_in.h"

#include <limits.h>

#include "audio/audio_codec_device.h"

/* At 24 dB, a 1 kHz tone clipped the mic above 10% volume; 6 dB keeps full volume below full scale. */
#define AUDIO_IN_GAIN_DB 6.0F

static esp_codec_dev_handle_t input;

bool
audio_in_open(void) {
    if (input) {
        return true;
    }
    input = audio_codec_device_open(ESP_CODEC_DEV_TYPE_IN);
    if (!input) {
        return false;
    }
    if (!audio_codec_check("microphone gain", esp_codec_dev_set_in_gain(input, AUDIO_IN_GAIN_DB))) {
        audio_in_close();
        return false;
    }
    return true;
}

void
audio_in_close(void) {
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
