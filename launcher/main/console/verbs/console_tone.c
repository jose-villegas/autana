/* Development-only acoustic tone check on the console task. */
#include <stdio.h>

#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "console/console_verbs.h"
#include "console/verbs/console_tone_parse.h"
#include "core/memory.h"
#include "core/timing.h"
#include "sound/sound_engine.h"

static void
stop_tone(void* context) {
    sound_voice_t* voice = context;
    sound_stop(*voice);
    *voice = SOUND_VOICE_NULL;
}

static bool
start_tone(void* context, uint16_t hz, uint8_t gain) {
    sound_voice_t* voice = context;
    *voice = sound_play_tone(hz, gain);
    const int64_t deadline = timing_now_us() + (int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS;
    while (sound_is_playing(*voice) && !sound_voice_audible(*voice) && timing_now_us() < deadline) {
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
    if (!sound_voice_audible(*voice)) {
        stop_tone(context);
        return false;
    }
    return true;
}

static void
console_verb_tone(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    if (!console_tone_parse(args, &parsed)) {
        reply("TONE usage: tone <hz> <ms> [volume]");
        return;
    }
    const size_t dma_before = memory_free_bytes(MEMORY_DMA);
    if (!sound_capture_open()) {
        (void)sound_capture_close();
        reply("TONE error: codec open failed");
        return;
    }
    const size_t dma_open = dma_before - memory_free_bytes(MEMORY_DMA);
    audio_loopback_t result;
    sound_voice_t voice = SOUND_VOICE_NULL;
    const audio_loopback_source_t source = {.context = &voice, .start = start_tone, .stop = stop_tone};
    const bool ok = audio_loopback_check(parsed.hz, parsed.ms, parsed.volume_pct, &source, &result);
    const bool closed = sound_capture_close();
    if (!ok || !closed) {
        reply("TONE error: loopback failed");
        return;
    }
    (void)printf("TONE %s %u Hz: %.1f dB (silence %.1f dB), open %zu B DMA\n", result.heard ? "heard" : "not heard",
                 (unsigned)parsed.hz, (double)result.tone_db, (double)result.silence_db, dma_open);
    (void)fflush(stdout);
}
CONSOLE_VERB(tone, 0, console_verb_tone)
