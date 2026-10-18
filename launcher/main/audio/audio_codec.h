/* Shared codec bring-up, persistent TX and capture-only RX. Callers serialize capture changes against TX writes. */
#pragma once
#include <stdbool.h>

#include "esp_codec_dev.h"

#define AUDIO_CODEC_BLOCK_SAMPLES   128u
#define AUDIO_CODEC_DMA_BLOCKS      4u
#define AUDIO_CODEC_BITS_PER_SAMPLE 16
#define AUDIO_CODEC_CHANNELS        1

const char* audio_codec_failure(void);
void audio_codec_assert_not_frame_task(void);
bool audio_codec_check(const char* operation, int status);
esp_codec_dev_handle_t audio_codec_device_open(esp_codec_dev_type_t type);
void audio_codec_device_close(esp_codec_dev_handle_t device);
