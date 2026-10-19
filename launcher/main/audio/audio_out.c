/* Blocking PCM output, used from a worker or console task. */
#include "audio/audio_out.h"

#include <limits.h>

#include "audio/audio_codec.h"
#include "audio/audio_out_device.h"
#include "board/board.h"

#define AUDIO_OUT_CODEC_VOLUME 30
static esp_codec_dev_handle_t output;
#define AUDIO_OUT_DAC_RAMP_REGISTER 0x37
#define AUDIO_OUT_DAC_RAMP_SHIFT    4u
#define AUDIO_OUT_DAC_RAMP_MASK     (AUDIO_OUT_DAC_RAMP_MAX << AUDIO_OUT_DAC_RAMP_SHIFT)
static bool amp_mute_enabled;
static bool amp_ready;
static uint8_t dac_ramp_rate;

bool
audio_out_amp_configure(bool mute, uint8_t ramp) {
    if (!output || ramp > AUDIO_OUT_DAC_RAMP_MAX) {
        return false;
    }
    if (ramp != dac_ramp_rate) {
        int value;
        if (!audio_codec_check("DAC ramp read", esp_codec_dev_read_reg(output, AUDIO_OUT_DAC_RAMP_REGISTER, &value))
            || !audio_codec_check("DAC ramp write",
                                  esp_codec_dev_write_reg(output, AUDIO_OUT_DAC_RAMP_REGISTER,
                                                          (value & ~(int)AUDIO_OUT_DAC_RAMP_MASK)
                                                              | (ramp << AUDIO_OUT_DAC_RAMP_SHIFT)))) {
            return false;
        }
        dac_ramp_rate = ramp;
    }
    if (mute != amp_mute_enabled) {
        if (!audio_codec_check("DAC mute", esp_codec_dev_set_out_mute(output, mute && !amp_ready))) {
            return false;
        }
        amp_mute_enabled = mute;
    }
    return true;
}

bool
audio_out_amp_ready(void) {
    if (amp_mute_enabled && !audio_codec_check("DAC unmute", esp_codec_dev_set_out_mute(output, false))) {
        return false;
    }
    amp_ready = true;
    return true;
}

bool
audio_out_amp_enable(bool enabled) {
    if (amp_mute_enabled && !audio_codec_check("DAC mute", esp_codec_dev_set_out_mute(output, true))) {
        return false;
    }
    amp_ready = false;
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
    amp_mute_enabled = false;
    amp_ready = false;
    dac_ramp_rate = 0;
}
