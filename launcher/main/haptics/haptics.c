#include "haptics/haptics.h"

#include "sound/sound_engine.h"

static sound_voice_t pulse;

void
haptic_pulse(haptic_t strength) {
    sound_stop(pulse);
    const bool firm = strength == HAPTIC_FIRM;
    pulse = sound_play_tone(HAPTIC_HZ, firm ? HAPTIC_FIRM_MS : HAPTIC_LIGHT_MS,
                            firm ? HAPTIC_FIRM_GAIN_PCT : HAPTIC_LIGHT_GAIN_PCT, SOUND_BUS_HAPTIC);
}
