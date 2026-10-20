#include "audio/audio_loopback.h"

#include <math.h>

#include "audio/audio_codec.h"
#include "audio/audio_frequency.h"
#include "audio/audio_in.h"

typedef struct {
    goertzel_t fundamental;
    goertzel_t harmonics[AUDIO_LOOPBACK_HARMONICS - AUDIO_LOOPBACK_FIRST_HARMONIC + 1u];
    audio_frequency_t frequency;
    size_t harmonic_count;
    int peak;
} audio_loopback_detector_t;

static void
detector_init(audio_loopback_detector_t* detector, uint16_t hz, bool analyze) {
    *detector = (audio_loopback_detector_t){0};
    goertzel_init(&detector->fundamental, (float)hz, (float)AUDIO_OUT_SAMPLE_RATE_HZ);
    audio_frequency_init(&detector->frequency, hz);
    if (!analyze) {
        return;
    }
    for (unsigned harmonic = AUDIO_LOOPBACK_FIRST_HARMONIC;
         harmonic <= AUDIO_LOOPBACK_HARMONICS && harmonic * hz < AUDIO_OUT_NYQUIST_HZ; harmonic++) {
        goertzel_init(&detector->harmonics[detector->harmonic_count++], (float)(harmonic * hz),
                      (float)AUDIO_OUT_SAMPLE_RATE_HZ);
    }
}

static void
detector_feed(audio_loopback_detector_t* detector, const int16_t* samples, size_t count, bool analyze) {
    goertzel_feed_s(&detector->fundamental, samples, count);
    audio_frequency_feed(&detector->frequency, samples, count);
    if (!analyze) {
        return;
    }
    for (size_t harmonic = 0; harmonic < detector->harmonic_count; harmonic++) {
        goertzel_feed_s(&detector->harmonics[harmonic], samples, count);
    }
    for (size_t sample = 0; sample < count; sample++) {
        const int magnitude = samples[sample] < 0 ? -(int)samples[sample] : samples[sample];
        if (magnitude > detector->peak) {
            detector->peak = magnitude;
        }
    }
}

static void
detector_result(const audio_loopback_detector_t* detector, audio_loopback_result_t* result) {
    result->measured_hz = audio_frequency_hz(&detector->frequency);
    float harmonic_power = 0;
    for (size_t harmonic = 0; harmonic < detector->harmonic_count; harmonic++) {
        harmonic_power += goertzel_power(&detector->harmonics[harmonic]);
    }
    const float fundamental = goertzel_power(&detector->fundamental);
    result->thd_db =
        detector->harmonic_count && fundamental > 0
            ? fmaxf(GOERTZEL_LEVEL_FLOOR_DB, AUDIO_LOOPBACK_POWER_DB_SCALE * log10f(harmonic_power / fundamental))
            : NAN;
    result->peak_dbfs = detector->peak ? fmaxf(GOERTZEL_LEVEL_FLOOR_DB, AUDIO_LOOPBACK_AMPLITUDE_DB_SCALE
                                                                            * log10f((float)detector->peak / INT16_MAX))
                                       : GOERTZEL_LEVEL_FLOOR_DB;
}

bool
audio_loopback_measure(uint16_t hz, uint16_t ms, float* db, audio_loopback_result_t* result) {
    if (!db || !audio_out_args_valid(hz, ms, 0)) {
        return false;
    }
    int16_t in[AUDIO_CODEC_BLOCK_SAMPLES];
    audio_loopback_detector_t detector;
    detector_init(&detector, hz, result != NULL);
    size_t remaining = audio_loopback_sample_budget(ms);
    size_t settle = audio_out_samples(AUDIO_LOOPBACK_SETTLE_MS);
    bool ok = true;
    while (remaining && ok) {
        const size_t n = remaining < AUDIO_CODEC_BLOCK_SAMPLES ? remaining : AUDIO_CODEC_BLOCK_SAMPLES;
        ok = audio_in_read(in, n);
        if (ok) {
            const size_t skipped = settle < n ? settle : n;
            settle -= skipped;
            detector_feed(&detector, in + skipped, n - skipped, result != NULL);
            remaining -= n;
        }
    }
    *db = goertzel_level_db(&detector.fundamental, INT16_MAX);
    if (result) {
        detector_result(&detector, result);
    }
    return ok;
}
