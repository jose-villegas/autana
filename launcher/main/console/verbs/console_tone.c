/* Development-only acoustic tone check on the console task. */
#include <stdio.h>

#include "audio/audio_in.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "console/console_verbs.h"
#include "console/verbs/console_tone_parse.h"

static void
console_verb_tone(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    if (!console_tone_parse(args, &parsed)) {
        reply("usage: tone <hz> <ms> [volume]");
        return;
    }
    if (!audio_out_open() || !audio_in_open()) {
        audio_in_close();
        audio_out_close();
        reply("TONE error: codec open failed");
        return;
    }
    audio_loopback_t result;
    if (!audio_loopback_check(parsed.hz, parsed.ms, parsed.volume_pct, &result)) {
        reply("TONE error: loopback failed");
        return;
    }
    (void)printf("TONE %s %u Hz: %.1f dB (silence %.1f dB)\n", result.heard ? "heard" : "not heard",
                 (unsigned)parsed.hz, (double)result.tone_db, (double)result.silence_db);
    (void)fflush(stdout);
}
CONSOLE_VERB(tone, 0, console_verb_tone)
