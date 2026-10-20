#include <math.h>
#include "console/verbs/console_melody.h"
#include "console/verbs/console_tone_parse.h"
#include "suites.h"
#include "unity.h"

#define TEST_MELODY_BARS             4u
#define TEST_MELODY_QUARTERS_PER_BAR 3u
#define TEST_MELODY_MAX_CENTS        5.0F

static melody_note_t
fixture(size_t index) {
    return melody_notes[index];
}

static void
test_melody_phrase_names_and_rhythm(void) {
    static const char* const expected[] = {"E5", "D5", "F#4", "G#4", "C#5", "B4", "D4",
                                           "E4", "B4", "A4",  "C#4", "E4",  "A4"};
    TEST_ASSERT_EQUAL_UINT(sizeof expected / sizeof expected[0], MELODY_PHRASE_NOTES);
    unsigned steps = 0;
    unsigned duration_ms = 0;
    for (size_t i = 0; i < MELODY_PHRASE_NOTES; i++) {
        const melody_note_t note = fixture(i);
        char name[MELODY_NAME_BYTES];
        melody_note_name(note, name);
        TEST_ASSERT_EQUAL_STRING(expected[i], name);
        steps += note.steps;
        duration_ms += note.steps * MELODY_STEP_MS;
        TEST_ASSERT_TRUE(
            audio_out_args_valid(melody_note_hz(note), CONSOLE_MELODY_CHECK_MS, CONSOLE_TONE_DEFAULT_VOLUME_PCT));
        const float cents =
            PITCH_CENTS_PER_SEMITONE * (pitch_hz_semitones(MELODY_REFERENCE_HZ, melody_note_hz(note)) - note.semitones);
        TEST_ASSERT_LESS_THAN_FLOAT(TEST_MELODY_MAX_CENTS, fabsf(cents));
        TEST_ASSERT_GREATER_THAN_UINT(MELODY_GAP_MS, note.steps * MELODY_STEP_MS);
    }
    TEST_ASSERT_LESS_THAN_UINT(CONSOLE_MELODY_PLAY_MAX_MS, duration_ms);
    TEST_ASSERT_EQUAL_UINT(TEST_MELODY_BARS * TEST_MELODY_QUARTERS_PER_BAR * MELODY_STEPS_PER_QUARTER, steps);
}

void
run_console_melody_suite(void) {
    RUN_TEST(test_melody_phrase_names_and_rhythm);
}
SUITE_REGISTER(run_console_melody_suite)
