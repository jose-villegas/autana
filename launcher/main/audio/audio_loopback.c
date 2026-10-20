#include "audio/audio_loopback.h"

#include "audio/audio_codec.h"

#include <math.h>

#include "audio/audio_frequency.h"
#include "audio/audio_in.h"

bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, audio_loopback_result_t* result) {
    if (!db || !audio_out_args_valid(hz, ms, 0)) {
        return false;
    }
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    goertzel_t detector;
    goertzel_init(&detector, (float)hz, (float)AUDIO_OUT_SAMPLE_RATE_HZ);
    goertzel_t harmonics[AUDIO_LOOPBACK_HARMONICS - AUDIO_LOOPBACK_FIRST_HARMONIC + 1u];
    size_t harmonic_count = 0;
    if (result) {
        for (unsigned harmonic = AUDIO_LOOPBACK_FIRST_HARMONIC;
             harmonic <= AUDIO_LOOPBACK_HARMONICS && harmonic * hz < AUDIO_OUT_NYQUIST_HZ; harmonic++) {
            goertzel_init(&harmonics[harmonic_count++], (float)(harmonic * hz), (float)AUDIO_OUT_SAMPLE_RATE_HZ);
        }
    }
    int peak = 0;
    audio_frequency_t frequency;
    audio_frequency_init(&frequency, hz);
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
            audio_frequency_feed(&frequency, in + skipped, n - skipped);
            if (result) {
                for (size_t harmonic = 0; harmonic < harmonic_count; harmonic++) {
                    goertzel_feed_s(&harmonics[harmonic], in + skipped, n - skipped);
                }
                for (size_t sample = skipped; sample < n; sample++) {
                    const int magnitude = in[sample] < 0 ? -(int)in[sample] : in[sample];
                    if (magnitude > peak) {
                        peak = magnitude;
                    }
                }
            }
            remaining -= n;
        }
    }
    *db = goertzel_level_db(&detector, INT16_MAX);
    if (result) {
        result->measured_hz = audio_frequency_hz(&frequency);
        float harmonic_power = 0;
        for (size_t harmonic = 0; harmonic < harmonic_count; harmonic++) {
            harmonic_power += goertzel_power(&harmonics[harmonic]);
        }
        const float fundamental = goertzel_power(&detector);
        result->thd_db =
            harmonic_count && fundamental > 0
                ? fmaxf(GOERTZEL_LEVEL_FLOOR_DB, AUDIO_LOOPBACK_POWER_DB_SCALE * log10f(harmonic_power / fundamental))
                : NAN;
        result->peak_dbfs =
            peak ? fmaxf(GOERTZEL_LEVEL_FLOOR_DB, AUDIO_LOOPBACK_AMPLITUDE_DB_SCALE * log10f((float)peak / INT16_MAX))
                 : GOERTZEL_LEVEL_FLOOR_DB;
    }
    return ok;
}
