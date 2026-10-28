/* console_haptic: development pulse latency check. */
#include <stdio.h>

#include "console/console_verbs.h"
#include "core/timing.h"
#include "haptics/haptics.h"
#include "sound/sound_engine.h"

#define HAPTIC_REPLY_BYTES 80

static void
console_verb_haptic(const char* args, console_reply_fn reply) {
    haptic_t strength;
    if (console_name_cmp(args, "light") == 0) {
        strength = HAPTIC_LIGHT;
    } else if (console_name_cmp(args, "firm") == 0) {
        strength = HAPTIC_FIRM;
    } else {
        reply("HAPTIC usage: haptic <light|firm>");
        return;
    }
    const int64_t began = timing_now_us();
    haptic_pulse(strength);
    sound_stats_t stats;
    sound_get_stats(&stats);
    sound_voice_t voice = SOUND_VOICE_NULL;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        if (stats.control.voices[i].playing && stats.control.voices[i].opts.bus.id == SOUND_BUS_HAPTIC.id) {
            voice = stats.control.voices[i].handle;
        }
    }
    const int64_t deadline = began + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    while (sound_is_playing(voice) && !sound_voice_audible(voice) && timing_now_us() < deadline) {
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
    if (!sound_voice_audible(voice)) {
        reply("HAPTIC error: pulse not audible");
        return;
    }
    char line[HAPTIC_REPLY_BYTES];
    (void)snprintf(line, sizeof line, "HAPTIC %s audible_ms=%lld", strength == HAPTIC_FIRM ? "firm" : "light",
                   (long long)((timing_now_us() - began) / MATH_US_PER_MS));
    reply(line);
}
CONSOLE_VERB(haptic, sizeof("light") - 1, console_verb_haptic)
