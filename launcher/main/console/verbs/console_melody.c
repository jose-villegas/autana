#include "console/verbs/console_melody.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include "console/console_verbs.h"
#include "console/verbs/console_tone.h"
#include "core/timing.h"

#define MELODY_PHRASE(NOTE)                                                                                            \
    NOTE(E, 5, MELODY_SHORT)                                                                                           \
    NOTE(D, 5, MELODY_SHORT)                                                                                           \
    NOTE(FS, 4, MELODY_LONG)                                                                                           \
    NOTE(GS, 4, MELODY_LONG)                                                                                           \
    NOTE(CS, 5, MELODY_SHORT)                                                                                          \
    NOTE(B, 4, MELODY_SHORT)                                                                                           \
    NOTE(D, 4, MELODY_LONG)                                                                                            \
    NOTE(E, 4, MELODY_LONG)                                                                                            \
    NOTE(B, 4, MELODY_SHORT)                                                                                           \
    NOTE(A, 4, MELODY_SHORT)                                                                                           \
    NOTE(CS, 4, MELODY_LONG)                                                                                           \
    NOTE(E, 4, MELODY_LONG)                                                                                            \
    NOTE(A, 4, MELODY_FINAL)

#define MELODY_NOTE(cls, octave, length)                                                                               \
    {.semitones = ((octave) - MELODY_REFERENCE_OCTAVE) * PITCH_SEMITONES_PER_OCTAVE + MELODY_CLASS_##cls               \
                  - MELODY_REFERENCE_CLASS,                                                                            \
     .steps = (length)},
#define MELODY_NOTE_STEPS(cls, octave, length) +(length)

/* Gran Vals, Francisco Tarrega, 1902; public domain. */
const melody_note_t melody_notes[] = {MELODY_PHRASE(MELODY_NOTE)};
_Static_assert(sizeof melody_notes / sizeof melody_notes[0] == MELODY_PHRASE_NOTES, "phrase note count");
_Static_assert((0 MELODY_PHRASE(MELODY_NOTE_STEPS)) * MELODY_STEP_MS < CONSOLE_MELODY_PLAY_MAX_MS,
               "phrase fits play timeout");

#define MELODY_CHECK_TOKEN "check"
#define MELODY_REPLY_BYTES (CONSOLE_TONE_REPLY_BYTES + MELODY_NAME_BYTES + CONSOLE_LINE_MAX)

static bool
parse_args(const char* args, console_tone_args_t* parsed, bool* check) {
    while (isspace((unsigned char)*args)) {
        args++;
    }
    *check = strncmp(args, MELODY_CHECK_TOKEN, sizeof MELODY_CHECK_TOKEN - 1) == 0
             && (!args[sizeof MELODY_CHECK_TOKEN - 1] || isspace((unsigned char)args[sizeof MELODY_CHECK_TOKEN - 1]));
    if (*check) {
        args += sizeof MELODY_CHECK_TOKEN - 1;
    }
    parsed->ms = CONSOLE_MELODY_CHECK_MS;
    if (!console_tone_parse_options(args, parsed) || (!*check && parsed->internal)) {
        return false;
    }
    return true;
}

static void
sleep_until(int64_t boundary_us) {
    const int64_t remaining_us = boundary_us - timing_now_us();
    if (remaining_us > 0) {
        timing_sleep_ms((uint32_t)((remaining_us + MATH_US_PER_MS - 1) / MATH_US_PER_MS));
    }
}

static const char*
play_note(melody_note_t note, uint8_t volume_pct, int64_t boundary_us) {
    sound_voice_t voice = SOUND_VOICE_NULL;
    if (!console_tone_start(&voice, melody_note_hz(note), volume_pct)) {
        return "tone start failed";
    }
    sleep_until(boundary_us - (int64_t)MELODY_GAP_MS * MATH_US_PER_MS);
    const bool stopped = console_tone_stop(&voice);
    sleep_until(boundary_us);
    return stopped ? NULL : "tone stop failed";
}

static const char*
check_note(melody_note_t note, const console_tone_args_t* args, console_reply_fn reply) {
    char tone[CONSOLE_TONE_REPLY_BYTES];
    const char* error = console_tone_check(args, tone);
    if (error) {
        return error;
    }
    char line[MELODY_REPLY_BYTES];
    char name[MELODY_NAME_BYTES];
    melody_note_name(note, name);
    (void)snprintf(line, sizeof line, "MELODY note=%s %s", name, tone);
    reply(line);
    return NULL;
}

static void
console_verb_melody(const char* args, console_reply_fn reply) {
    console_tone_args_t parsed;
    bool check;
    if (!parse_args(args, &parsed, &check)) {
        reply("MELODY_ERR usage: melody [volume] | melody check [volume] [internal]");
        return;
    }
    char line[MELODY_REPLY_BYTES];
    const int64_t started = timing_now_us();
    unsigned cumulative_steps = 0;
    for (size_t i = 0; i < MELODY_PHRASE_NOTES; i++) {
        const melody_note_t note = melody_notes[i];
        parsed.hz = melody_note_hz(note);
        cumulative_steps += note.steps;
        const int64_t boundary_us = started + (int64_t)cumulative_steps * MELODY_STEP_MS * MATH_US_PER_MS;
        const char* error = check ? check_note(note, &parsed, reply) : play_note(note, parsed.volume_pct, boundary_us);
        if (error) {
            (void)snprintf(line, sizeof line, "MELODY_ERR %s", error);
            reply(line);
            return;
        }
    }
    if (check) {
        (void)snprintf(line, sizeof line, "MELODY_END notes=%u", MELODY_PHRASE_NOTES);
    } else {
        (void)snprintf(line, sizeof line, "MELODY_END notes=%u ms=%" PRId64, MELODY_PHRASE_NOTES,
                       (timing_now_us() - started) / MATH_US_PER_MS);
    }
    reply(line);
}
CONSOLE_VERB(melody, 0, console_verb_melody)
