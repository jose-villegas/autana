/* Sample-rate owner for blocking mono PCM output. The amplifier is off when
 * idle; callers serialize audio operations and keep them off the frame loop. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_OUT_SAMPLE_RATE_HZ 22050
#define AUDIO_OUT_NYQUIST_HZ     (AUDIO_OUT_SAMPLE_RATE_HZ / 2)
#define AUDIO_OUT_MAX_MS         60000u
#define AUDIO_OUT_VOLUME_MAX_PCT 100u
#define AUDIO_OUT_MS_PER_SECOND  1000u

bool audio_out_open(void);
/* Blocking; never call from the frame loop (the main task). */
bool audio_out_tone(uint16_t hz, uint16_t ms, uint8_t volume_pct);
/* The last input/output close releases the shared codec and I2S channels. */
void audio_out_close(void);
