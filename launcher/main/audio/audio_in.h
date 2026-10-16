/* Blocking mono microphone capture. Callers serialize audio operations. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool audio_in_open(void);
void audio_in_close(void);
bool audio_in_read(int16_t* samples, size_t count);
