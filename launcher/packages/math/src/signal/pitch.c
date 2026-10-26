#include "math/signal/pitch.h"
#include <math.h>

float
pitch_semitone_hz(float reference_hz, float semitones) {
    return reference_hz * exp2f(semitones / PITCH_SEMITONES_PER_OCTAVE);
}

float
pitch_hz_semitones(float reference_hz, float hz) {
    return hz <= 0 || reference_hz <= 0 ? NAN : PITCH_SEMITONES_PER_OCTAVE * log2f(hz / reference_hz);
}
