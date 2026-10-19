/* Development-only tone check on the console task. */
#include <math.h>
#include <stdio.h>

#include "audio/audio_codec_feedback.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "audio/audio_out_device.h"
#include "console/console_verbs.h"
#include "console/verbs/console_tone_parse.h"
#include "console/verbs/console_tone_reply.h"
#include "core/memory.h"
#include "core/timing.h"
#include "sound/sound_engine.h"

static bool
stop_tone(sound_voice_t* voice) {
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    while (sound_is_playing(*voice) && timing_now_us() < deadline) {
        sound_stop(*voice);
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
    const bool stopped = !sound_is_playing(*voice);
    *voice = SOUND_VOICE_NULL;
    return stopped;
}

static bool
start_tone(sound_voice_t* voice, uint16_t hz, uint8_t gain) {
    *voice = sound_play_tone(hz, gain);
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    while (sound_is_playing(*voice) && !sound_voice_audible(*voice) && timing_now_us() < deadline) {
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
    if (!sound_voice_audible(*voice)) {
        stop_tone(voice);
        return false;
    }
    return true;
}

static void
console_verb_tone(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    if (!console_tone_parse(args, &parsed)) {
        reply("TONE usage: tone <hz> <ms> [volume] [internal]");
        return;
    }
    const size_t dma_before = memory_free_bytes(MEMORY_DMA);
    const bool opened = sound_capture_open();
    const size_t dma_open = dma_before - memory_free_bytes(MEMORY_DMA);
    float silence_db = 0;
    float tone_db = 0;
    float measured_hz = parsed.hz;
    sound_voice_t voice = SOUND_VOICE_NULL;
    bool held = false;
    bool ok = opened && audio_codec_dac_feedback(false)
              && (!parsed.internal || ((held = audio_out_amp_hold(true)) && audio_codec_dac_feedback(true)))
              && audio_loopback_measure(parsed.hz, parsed.ms, &silence_db, NULL)
              && start_tone(&voice, parsed.hz, parsed.volume_pct)
              && audio_loopback_measure(parsed.hz, parsed.ms, &tone_db, &measured_hz);
    ok = stop_tone(&voice) && ok;
    if (held) {
        timing_sleep_ms(AUDIO_OUT_DMA_DRAIN_MS);
    }
    const bool heard = tone_db - silence_db >= AUDIO_LOOPBACK_HEARD_MARGIN_DB;
    ok = audio_codec_dac_feedback(false) && ok;
    ok = audio_out_amp_hold(false) && ok;
    ok = sound_capture_close() && ok;
    if (!ok) {
        reply(opened ? "TONE error: loopback failed" : "TONE error: codec open failed");
        return;
    }
    char line[CONSOLE_TONE_REPLY_BYTES];
    if (!console_tone_reply(line, sizeof line, &parsed, heard, tone_db, silence_db, measured_hz, dma_open)) {
        reply("TONE error: frequency formatting failed");
        return;
    }
    (void)printf("%s\n", line);
    (void)fflush(stdout);
}
CONSOLE_VERB(tone, 0, console_verb_tone)
