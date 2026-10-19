/* Private blocking PCM block output and board-owned amplifier control. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool audio_out_write(const int16_t* samples, size_t count);
#define AUDIO_OUT_AMP_STARTUP_MS 250

/* Enabling returns immediately; callers write silence throughout startup. */
bool audio_out_amp_enable(bool enabled);
bool audio_out_amp_hold(bool held);
