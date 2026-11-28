/* render_host_haptics: strong stand-in because some MinGW linkers leave weak calls unresolved. */
#include "haptics/haptics.h"

sound_voice_t
haptic_pulse(haptic_t strength) {
    (void)strength;
    return SOUND_VOICE_NULL;
}
