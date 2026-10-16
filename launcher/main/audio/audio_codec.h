/* Private codec lifetime shared by input and output references, releasing I2S on
 * the last close. The BSP audio helpers have no deinit. The codec PA pin is unused:
 * board_audio_amp_enable owns the amplifier. Callers serialize access. */
#pragma once
#include <stdbool.h>

#include "esp_codec_dev.h"

#define AUDIO_CODEC_BLOCK_SAMPLES   128u
#define AUDIO_CODEC_DMA_BLOCKS      4u
#define AUDIO_CODEC_BITS_PER_SAMPLE 16
#define AUDIO_CODEC_CHANNELS        1

bool audio_codec_check(const char* operation, int status);
esp_codec_dev_handle_t audio_codec_device_open(esp_codec_dev_type_t type);
void audio_codec_device_close(esp_codec_dev_handle_t device);
