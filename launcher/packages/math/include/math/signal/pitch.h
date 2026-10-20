/* pitch: equal-temperament offsets relative to a caller's reference frequency. */
#pragma once

#define PITCH_SEMITONES_PER_OCTAVE 12
#define PITCH_CENTS_PER_SEMITONE   100

/* hz = reference_hz * 2^(semitones / PITCH_SEMITONES_PER_OCTAVE). */
float pitch_semitone_hz(float reference_hz, float semitones);
/* Returns NAN when hz <= 0 or reference_hz <= 0. */
float pitch_hz_semitones(float reference_hz, float hz);
