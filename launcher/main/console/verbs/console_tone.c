/* Development-only tone check on the console task. */
#include <math.h>
#include <stdio.h>

#include "audio/audio_codec.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out.h"
#include "audio/audio_out_device.h"
#include "console/console_verbs.h"
#include "console/verbs/console_tone_parse.h"
#include "core/memory.h"
#include "core/timing.h"
#include "sound/sound_engine.h"

#define TONE_MEASURED_TEXT_BYTES 32u

static void
stop_tone(sound_voice_t* voice) {
    sound_stop(*voice);
    *voice = SOUND_VOICE_NULL;
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
    if (!sound_capture_open()) {
        (void)sound_capture_close();
        reply("TONE error: codec open failed");
        return;
    }
    const size_t dma_open = dma_before - memory_free_bytes(MEMORY_DMA);
    float silence_db = 0;
    float tone_db = 0;
    float measured_hz = parsed.hz;
    sound_voice_t voice = SOUND_VOICE_NULL;
    const bool ok = (!parsed.internal || (audio_out_amp_hold(true) && audio_codec_dac_feedback(true)))
                    && audio_loopback_measure(parsed.hz, parsed.ms, &silence_db, NULL)
                    && start_tone(&voice, parsed.hz, parsed.volume_pct)
                    && audio_loopback_measure(parsed.hz, parsed.ms, &tone_db, &measured_hz);
    stop_tone(&voice);
    const bool heard = tone_db - silence_db >= AUDIO_LOOPBACK_HEARD_MARGIN_DB;
    const bool closed = sound_capture_close();
    if (parsed.internal && !closed) {
        (void)audio_codec_dac_feedback(false);
        (void)audio_out_amp_hold(false);
    }
    if (!ok || !closed) {
        reply("TONE error: loopback failed");
        return;
    }
    char measured[TONE_MEASURED_TEXT_BYTES];
    const int measured_length = isfinite(measured_hz)
                                    ? snprintf(measured, sizeof measured, "%.2f Hz", (double)measured_hz)
                                    : snprintf(measured, sizeof measured, "n/a");
    if (measured_length < 0 || (size_t)measured_length >= sizeof measured) {
        reply("TONE error: frequency formatting failed");
        return;
    }
    (void)printf("TONE %s %u Hz: %.1f dB (silence %.1f dB), path %s, measured %s, open %zu B DMA\n",
                 heard ? "heard" : "not heard", (unsigned)parsed.hz, (double)tone_db, (double)silence_db,
                 parsed.internal ? "internal" : "acoustic", measured, dma_open);
    (void)fflush(stdout);
}
CONSOLE_VERB(tone, 0, console_verb_tone)
