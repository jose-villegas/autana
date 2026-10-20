/* console_melody: a reference-relative phrase and its rhythm. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "math/scalar/units.h"
#include "math/signal/pitch.h"

typedef enum {
    MELODY_CLASS_C,
    MELODY_CLASS_CS,
    MELODY_CLASS_D,
    MELODY_CLASS_DS,
    MELODY_CLASS_E,
    MELODY_CLASS_F,
    MELODY_CLASS_FS,
    MELODY_CLASS_G,
    MELODY_CLASS_GS,
    MELODY_CLASS_A,
    MELODY_CLASS_AS,
    MELODY_CLASS_B
} melody_pitch_class_t;

typedef struct {
    int8_t semitones;
    uint8_t steps;
} melody_note_t;

#define MELODY_REFERENCE_HZ        440.0F
#define MELODY_REFERENCE_CLASS     MELODY_CLASS_A
#define MELODY_REFERENCE_OCTAVE    4
/* Note lengths in steps of MELODY_STEP_MS. */
#define MELODY_SHORT               1u
#define MELODY_LONG                2u
#define MELODY_FINAL               6u
#define MELODY_TEMPO_QPM           180u
#define MELODY_STEPS_PER_QUARTER   2u
#define MELODY_STEP_MS             (MATH_MS_PER_MIN / (MELODY_TEMPO_QPM * MELODY_STEPS_PER_QUARTER))
#define MELODY_GAP_MS              20u
#define MELODY_NAME_BYTES          8u
/* Each note is captured for at least two frequency windows; 200 ms leaves several, so the phase estimate is averaged. */
#define CONSOLE_MELODY_CHECK_MS    200u
#define MELODY_PHRASE_NOTES        13u
#define CONSOLE_MELODY_PLAY_MAX_MS 5000u

extern const melody_note_t melody_notes[];

static inline uint16_t
melody_note_hz(melody_note_t note) {
    return (uint16_t)lroundf(pitch_semitone_hz(MELODY_REFERENCE_HZ, note.semitones));
}

static inline void
melody_note_name(melody_note_t note, char name[MELODY_NAME_BYTES]) {
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int absolute = note.semitones + MELODY_REFERENCE_CLASS + MELODY_REFERENCE_OCTAVE * PITCH_SEMITONES_PER_OCTAVE;
    const int pitch_class =
        (absolute % PITCH_SEMITONES_PER_OCTAVE + PITCH_SEMITONES_PER_OCTAVE) % PITCH_SEMITONES_PER_OCTAVE;
    (void)snprintf(name, MELODY_NAME_BYTES, "%s%d", names[pitch_class],
                   (absolute - pitch_class) / PITCH_SEMITONES_PER_OCTAVE);
}
