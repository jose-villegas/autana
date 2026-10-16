/* Private blocking PCM block output and board-owned amplifier control. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_OUT_BLOCK_SAMPLES 128u
#define AUDIO_OUT_DMA_BLOCKS    4u
bool audio_out_write(const int16_t* samples, size_t count);
bool audio_out_amp(bool enabled);
