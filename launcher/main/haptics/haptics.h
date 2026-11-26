/* haptics: named speaker pulses that shake the case. */
#pragma once

#include "sound/sound_play.h"

/* Case resonance measured in docs/audio/Low-Frequency-Buzz.md. */
#define HAPTIC_HZ             202
#define HAPTIC_LIGHT_GAIN_PCT 30
#define HAPTIC_FIRM_GAIN_PCT  60
/* Firm is the shortest pulse whose median shake reaches 90% of the 400 ms tone's
 * in every run (docs/haptics/README.md). Light shares its duration at lower gain. */
#define HAPTIC_LIGHT_MS       100
#define HAPTIC_FIRM_MS        100

typedef enum { HAPTIC_LIGHT, HAPTIC_FIRM } haptic_t;

sound_voice_t haptic_pulse(haptic_t strength);
