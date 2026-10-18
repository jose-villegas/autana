/* Capture and detection do not own playback. */
#include "audio/audio_loopback.h"

#include "audio/audio_codec.h"
#include "audio/audio_in.h"
#include "math/scalar/trig.h"
#include "math/signal/goertzel.h"

bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db) {
    if (!db || !audio_out_args_valid(hz, ms, 0)) {
        return false;
    }
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    goertzel_t detector;
    goertzel_init(&detector, (float)hz, (float)AUDIO_OUT_SAMPLE_RATE_HZ);
    size_t remaining = audio_loopback_sample_budget(ms);
    size_t settle = audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS);
    bool ok = true;
    while (remaining && ok) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        ok = audio_in_read(in, n);
        if (ok) {
            const size_t skipped = settle < n ? settle : n;
            settle -= skipped;
            goertzel_feed_s(&detector, in + skipped, n - skipped);
            remaining -= n;
        }
    }
    *db = goertzel_level_db(&detector, TRIG_SIN_MAX);
    return ok;
}
