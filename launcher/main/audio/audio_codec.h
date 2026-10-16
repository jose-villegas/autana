/* Private shared codec lifetime. Each device retains one reference. Callers serialize access. */
#pragma once
#include <stdbool.h>
#include "esp_codec_dev.h"
#define AUDIO_CODEC_BITS_PER_SAMPLE 16
#define AUDIO_CODEC_CHANNELS        1
bool audio_codec_check(const char* operation, int status);
esp_codec_dev_handle_t audio_codec_device_open(esp_codec_dev_type_t type);
void audio_codec_device_close(esp_codec_dev_handle_t device);
