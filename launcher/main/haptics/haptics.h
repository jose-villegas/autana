/* haptics: named speaker pulses that shake the case. */
#pragma once

/* Case resonance measured in docs/audio/Low-Frequency-Buzz.md. */
#define HAPTIC_HZ             202
#define HAPTIC_LIGHT_GAIN_PCT 30
#define HAPTIC_FIRM_GAIN_PCT  60
/* Shortest pulse whose firm shake reaches the long tone's (docs/haptics/README.md);
 * light shares it, so the strengths differ by gain only. */
#define HAPTIC_LIGHT_MS       100
#define HAPTIC_FIRM_MS        100

typedef enum { HAPTIC_LIGHT, HAPTIC_FIRM } haptic_t;

void haptic_pulse(haptic_t strength);
