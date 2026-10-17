/* Development-only acoustic tone check on the console task. */
#include <stdio.h>

#include "audio/audio_in.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "console/console_verbs.h"
#include "console/verbs/console_tone_parse.h"
#include "core/memory.h"

static void
console_verb_tone(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    if (!console_tone_parse(args, &parsed)) {
        reply("TONE usage: tone <hz> <ms> [volume]");
        return;
    }
    const size_t dma_before = memory_free_bytes(MEMORY_DMA);
    if (!audio_out_open() || !audio_in_open()) {
        audio_in_close();
        audio_out_close();
        reply("TONE error: codec open failed");
        return;
    }
    const size_t dma_open = dma_before - memory_free_bytes(MEMORY_DMA);
    audio_loopback_t result;
    const bool ok = audio_loopback_check(parsed.hz, parsed.ms, parsed.volume_pct, &result);
    audio_in_close();
    audio_out_close();
    if (!ok) {
        reply("TONE error: loopback failed");
        return;
    }
    (void)printf("TONE %s %u Hz: %.1f dB (silence %.1f dB), open %zu B DMA\n", result.heard ? "heard" : "not heard",
                 (unsigned)parsed.hz, (double)result.tone_db, (double)result.silence_db, dma_open);
    (void)fflush(stdout);
}
CONSOLE_VERB(tone, 0, console_verb_tone)
