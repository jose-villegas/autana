/* Blocking PCM output, used from a worker or console task. */
#include "audio/audio_out.h"

#include <limits.h>

#include "audio/audio_codec.h"
#include "audio/audio_out_device.h"
#include "board/board.h"

#define AUDIO_OUT_CODEC_VOLUME 30
static esp_codec_dev_handle_t output;

bool
audio_out_amp_enable(bool enabled) {
    return audio_codec_check("amplifier", board_audio_amp_enable(enabled));
}

bool
audio_out_open(void) {
    if (output) {
        return true;
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
