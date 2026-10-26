/* Private blocking PCM block output and board-owned amplifier control. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_codec.h"
#include "audio/audio_out.h"

#define AUDIO_OUT_AMP_STARTUP_MS 250
#define AUDIO_OUT_DMA_DRAIN_MS                                                                                         \
    ((AUDIO_CODEC_DMA_BLOCKS * AUDIO_CODEC_BLOCK_SAMPLES * MATH_MS_PER_S + AUDIO_OUT_SAMPLE_RATE_HZ - 1u)              \
     / AUDIO_OUT_SAMPLE_RATE_HZ)

bool audio_out_write(const int16_t* samples, size_t count);

/* Enabling returns immediately; callers write silence throughout startup. */
bool audio_out_amp_enable(bool enabled);
bool audio_out_amp_hold(bool held);
