/* console_haptic: development pulse latency check. */
#include <stdio.h>

#include "console/console_verbs.h"
#include "core/timing.h"
#include "haptics/haptics.h"
#include "sound/sound_engine.h"

#define HAPTIC_REPLY_BYTES 80

static const char* const strength_names[] = {
    [HAPTIC_LIGHT] = "light",
    [HAPTIC_FIRM] = "firm",
};

static void
console_verb_haptic(const char* args, console_reply_fn reply) {
    const size_t strength_count = sizeof strength_names / sizeof strength_names[0];
    size_t strength = 0;
    while (strength < strength_count && console_name_cmp(args, strength_names[strength]) != 0) {
        ++strength;
    }
    if (strength == strength_count) {
        reply("HAPTIC usage: haptic <light|firm>");
        return;
    }
    const int64_t began = timing_now_us();
    const sound_voice_t voice = haptic_pulse((haptic_t)strength);
    if (!sound_voice_wait_audible(voice, SOUND_CAPTURE_TIMEOUT_MS)) {
        reply("HAPTIC error: pulse not audible");
        return;
    }
    char line[HAPTIC_REPLY_BYTES];
    (void)snprintf(line, sizeof line, "HAPTIC %s audible_ms=%lld", strength_names[strength],
                   (long long)((timing_now_us() - began) / MATH_US_PER_MS));
    reply(line);
}
CONSOLE_VERB(haptic, sizeof("light") - 1, console_verb_haptic)
