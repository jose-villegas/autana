/* Codec device handles for hardware-facing capture and playback. */
#pragma once
#include "esp_codec_dev.h"

#include "audio/audio_codec.h"

esp_codec_dev_handle_t audio_codec_device_open(esp_codec_dev_type_t type);
void audio_codec_device_close(esp_codec_dev_handle_t device);
