/* Raw codec access is confined to the development console. */
#include <stdio.h>

#include "audio/audio_codec.h"
#include "console/console_verbs.h"
#include "console/verbs/console_codec_parse.h"

#define CONSOLE_CODEC_REPLY_BYTES sizeof "CODEC reg=0xFF value=0xFF"

static void
console_verb_codec(const char* args, console_reply_fn reply) {
    console_codec_args_t parsed;
    if (!console_codec_parse(args, &parsed)) {
        reply("CODEC usage: codec <reg> [value]");
        return;
    }
    if (!audio_codec_register(parsed.reg, parsed.write, &parsed.value)) {
        reply("CODEC error: codec unavailable or register access failed");
        return;
    }
    char line[CONSOLE_CODEC_REPLY_BYTES];
    (void)snprintf(line, sizeof line, "CODEC reg=0x%02X value=0x%02X", (unsigned)parsed.reg, (unsigned)parsed.value);
    reply(line);
}
CONSOLE_VERB(codec, sizeof "0xFF 0xFF" - 1, console_verb_codec)
