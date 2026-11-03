#include "audio/audio_loopback_detector.h"

#include "math/scalar/math_db.h"

void
audio_loopback_detector_init(audio_loopback_detector_t* detector, uint16_t hz, bool analyze) {
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

void
audio_loopback_detector_feed(audio_loopback_detector_t* detector, const int16_t* samples, size_t count, bool analyze) {
    for (size_t i = 0; i < count; i++) {
        const int32_t sample = samples[i];
        detector->square_sum += (uint32_t)(sample * sample);
    }
    detector->sample_count += count;
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

void
audio_loopback_detector_result(const audio_loopback_detector_t* detector, audio_loopback_result_t* result) {
    result->noise_dbfs = detector->sample_count
                             ? math_db_power((float)detector->square_sum / (float)detector->sample_count
                                             / ((float)INT16_MAX * INT16_MAX))
                             : MATH_DB_FLOOR;
    result->measured_hz = audio_frequency_hz(&detector->frequency);
    float harmonic_power = 0;
    for (size_t harmonic = 0; harmonic < detector->harmonic_count; harmonic++) {
        harmonic_power += goertzel_power(&detector->harmonics[harmonic]);
    }
    const float fundamental = goertzel_power(&detector->fundamental);
    result->thd_db = detector->harmonic_count && fundamental > 0 ? math_db_power(harmonic_power / fundamental) : NAN;
    result->peak_dbfs = math_db_amplitude((float)detector->peak / INT16_MAX);
}
