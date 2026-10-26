#include "audio/audio_loopback.h"

#include "audio/audio_codec.h"
#include "audio/audio_in.h"
#include "audio/audio_loopback_detector.h"

bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, audio_loopback_result_t* result) {
    if (!db || !audio_out_args_valid(hz, ms, 0)) {
        return false;
    }
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    audio_loopback_detector_t detector;
    audio_loopback_detector_init(&detector, hz, result != NULL);
    size_t remaining = audio_loopback_sample_budget(ms);
    size_t settle = audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS);
    bool ok = true;
    while (remaining && ok) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        ok = audio_in_read(in, n);
        if (ok) {
            const size_t skipped = settle < n ? settle : n;
            settle -= skipped;
            audio_loopback_detector_feed(&detector, in + skipped, n - skipped, result != NULL);
            remaining -= n;
        }
    }
    *db = goertzel_level_db(&detector.fundamental, INT16_MAX);
    if (result) {
        audio_loopback_detector_result(&detector, result);
    }
    return ok;
}
