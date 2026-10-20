/* console_melody: a reference-relative phrase and its rhythm. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "math/scalar/units.h"
#include "math/signal/pitch.h"

typedef enum {
    PITCH_CLASS_C,
    PITCH_CLASS_CS,
    PITCH_CLASS_D,
    PITCH_CLASS_DS,
    PITCH_CLASS_E,
    PITCH_CLASS_F,
    PITCH_CLASS_FS,
    PITCH_CLASS_G,
    PITCH_CLASS_GS,
    PITCH_CLASS_A,
    PITCH_CLASS_AS,
    PITCH_CLASS_B
} melody_pitch_class_t;

typedef struct {
    int8_t semitones;
    uint8_t steps;
} melody_note_t;

#define MELODY_REFERENCE_HZ      440.0F
#define MELODY_REFERENCE_CLASS   PITCH_CLASS_A
#define MELODY_REFERENCE_OCTAVE  4
#define MELODY_SHORT             1u
#define MELODY_LONG              2u
#define MELODY_FINAL             6u
#define MELODY_TEMPO_QPM         180u
#define MELODY_STEPS_PER_QUARTER 2u
#define MELODY_STEP_MS           (MATH_MS_PER_MIN / (MELODY_TEMPO_QPM * MELODY_STEPS_PER_QUARTER))
#define MELODY_GAP_MS            20u
#define MELODY_NAME_BYTES        8u
/* Two frequency windows are needed; this capture leaves several windows for phase averaging. */
#define CONSOLE_MELODY_CHECK_MS  200u
#define MELODY_NOTE(cls, octave, length)                                                                               \
    {.semitones = ((octave) - MELODY_REFERENCE_OCTAVE) * PITCH_SEMITONES_PER_OCTAVE + PITCH_CLASS_##cls                \
                  - MELODY_REFERENCE_CLASS,                                                                            \
     .steps = (length)}

/* Gran Vals, Francisco Tarrega, 1902; public domain. */
static const melody_note_t melody_notes[] = {
    MELODY_NOTE(E, 5, MELODY_SHORT), MELODY_NOTE(D, 5, MELODY_SHORT),  MELODY_NOTE(FS, 4, MELODY_LONG),
    MELODY_NOTE(GS, 4, MELODY_LONG), MELODY_NOTE(CS, 5, MELODY_SHORT), MELODY_NOTE(B, 4, MELODY_SHORT),
    MELODY_NOTE(D, 4, MELODY_LONG),  MELODY_NOTE(E, 4, MELODY_LONG),   MELODY_NOTE(B, 4, MELODY_SHORT),
    MELODY_NOTE(A, 4, MELODY_SHORT), MELODY_NOTE(CS, 4, MELODY_LONG),  MELODY_NOTE(E, 4, MELODY_LONG),
    MELODY_NOTE(A, 4, MELODY_FINAL)};
#define MELODY_NOTE_COUNT (sizeof melody_notes / sizeof melody_notes[0])

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
