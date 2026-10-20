/* Shared codec bring-up, persistent TX and capture-only RX. Callers serialize capture changes against TX writes. */
#pragma once
#include <stdbool.h>

#define AUDIO_CODEC_BLOCK_SAMPLES   128u
#define AUDIO_CODEC_DMA_BLOCKS      4u
#define AUDIO_CODEC_BITS_PER_SAMPLE 16
#define AUDIO_CODEC_CHANNELS        1

const char* audio_codec_failure(void);
void audio_codec_assert_not_frame_task(void);
bool audio_codec_check(const char* operation, int status);
