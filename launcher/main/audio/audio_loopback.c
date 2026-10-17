/* Capture and detection stay in the driver; the caller supplies the playback owner. */
#include "audio/audio_loopback.h"

#include "audio/audio_codec.h"
#include "audio/audio_goertzel.h"
#include "audio/audio_in.h"

static bool
audio_loopback_record(uint16_t hz, uint16_t ms, uint8_t volume_pct, bool playing, const audio_loopback_source_t* source,
                      float* db) {
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    audio_goertzel_t detector;
    audio_goertzel_init(&detector, hz, AUDIO_OUT_SAMPLE_RATE_HZ);
    size_t remaining = audio_loopback_sample_budget(ms);
    size_t settle = audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS);
    if (playing && !source->start(source->context, hz, volume_pct)) {
        return false;
    }
    bool ok = true;
    while (remaining && ok) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        ok = audio_in_read(in, n);
        if (ok) {
            const size_t skipped = settle < n ? settle : n;
            settle -= skipped;
            audio_goertzel_feed(&detector, in + skipped, n - skipped);
            remaining -= n;
        }
    }
    if (playing) {
        source->stop(source->context);
    }
    *db = audio_goertzel_dbfs(&detector);
    return ok;
}

bool
audio_loopback_check(uint16_t hz, uint16_t ms, uint8_t volume_pct, const audio_loopback_source_t* source,
                     audio_loopback_t* result) {
    if (!result || !source || !source->start || !source->stop || !audio_out_args_valid(hz, ms, volume_pct)) {
        return false;
    }
    audio_loopback_t measured = {0};
    const bool ok = audio_loopback_record(hz, ms, volume_pct, false, source, &measured.silence_db)
                    && audio_loopback_record(hz, ms, volume_pct, true, source, &measured.tone_db);
    if (!ok) {
        return false;
    }
    measured.heard = measured.tone_db - measured.silence_db >= AUDIO_LOOPBACK_HEARD_MARGIN_DB;
    *result = measured;
    return true;
}
