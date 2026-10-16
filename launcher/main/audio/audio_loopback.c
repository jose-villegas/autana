/* One playback block and one capture block keep memory independent of duration. */
#include "audio/audio_loopback.h"

#include "audio/audio_goertzel.h"
#include "audio/audio_in.h"
#include "audio/audio_out.h"
#include "audio/audio_out_device.h"
#include "audio/audio_tone.h"

static bool
audio_loopback_start(audio_tone_t* tone, int16_t* out, bool playing, uint8_t volume_pct) {
    if (!audio_out_amp(playing)) {
        return false;
    }
    if (playing) {
        audio_tone_fill(tone, out, AUDIO_CODEC_BLOCK_SAMPLES, volume_pct);
        if (!audio_out_write(out, AUDIO_CODEC_BLOCK_SAMPLES)) {
            return false;
        }
    }
    return true;
}

static bool
audio_loopback_record(uint16_t hz, uint16_t ms, uint8_t volume_pct, bool playing, float* db) {
    int16_t out[AUDIO_CODEC_BLOCK_SAMPLES];
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    audio_goertzel_t detector;
    audio_goertzel_init(&detector, hz, AUDIO_OUT_SAMPLE_RATE_HZ);
    audio_tone_t tone;
    audio_tone_init(&tone, hz);
    size_t remaining = (uint32_t)ms * AUDIO_OUT_SAMPLE_RATE_HZ / AUDIO_OUT_MS_PER_SECOND;
    size_t settle = AUDIO_LOOPBACK_SETTLE_MS * AUDIO_OUT_SAMPLE_RATE_HZ / AUDIO_OUT_MS_PER_SECOND;
    if (!audio_loopback_start(&tone, out, playing, volume_pct)) {
        return false;
    }
    while (remaining) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        if (playing) {
            audio_tone_fill(&tone, out, n, volume_pct);
            if (!audio_out_write(out, n)) {
                return false;
            }
        }
        if (!audio_in_read(in, n)) {
            return false;
        }
        const size_t skipped = settle < n ? settle : n;
        settle -= skipped;
        audio_goertzel_feed(&detector, in + skipped, n - skipped);
        remaining -= n;
    }
    *db = audio_goertzel_dbfs(&detector);
    return true;
}

bool
audio_loopback_check(uint16_t hz, uint16_t ms, uint8_t volume_pct, audio_loopback_t* result) {
    if (!result || hz == 0 || hz >= AUDIO_OUT_NYQUIST_HZ || ms == 0 || ms > AUDIO_OUT_MAX_MS
        || volume_pct > AUDIO_OUT_VOLUME_MAX_PCT) {
        return false;
    }
    audio_loopback_t measured = {0};
    const bool ok = audio_loopback_record(hz, ms, volume_pct, false, &measured.silence_db)
                    && audio_loopback_record(hz, ms, volume_pct, true, &measured.tone_db);
    const bool off = audio_out_amp(false);
    if (!ok || !off) {
        return false;
    }
    measured.heard = measured.tone_db - measured.silence_db >= AUDIO_LOOPBACK_HEARD_MARGIN_DB;
    *result = measured;
    return true;
}
