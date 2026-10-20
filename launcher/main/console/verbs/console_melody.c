#include "console/verbs/console_melody.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include "console/console_verbs.h"
#include "console/verbs/console_tone.h"
#include "core/timing.h"

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
    char line[MELODY_REPLY_BYTES];
    const int length = snprintf(line, sizeof line, "%u %u %s", (unsigned)melody_note_hz(melody_notes[0]),
                                (unsigned)CONSOLE_MELODY_CHECK_MS, args);
    if (length < 0 || (size_t)length >= sizeof line || !console_tone_parse(line, parsed)
        || (!*check && parsed->internal)) {
        return false;
    }
    return true;
}

static const char*
play_note(melody_note_t note, uint8_t volume_pct) {
    sound_voice_t voice = SOUND_VOICE_NULL;
    if (!console_tone_start(&voice, melody_note_hz(note), volume_pct)) {
        return "tone start failed";
    }
    timing_sleep_ms((note.steps * MELODY_STEP_MS) - MELODY_GAP_MS);
    const bool stopped = console_tone_stop(&voice);
    timing_sleep_ms(MELODY_GAP_MS);
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
    for (size_t i = 0; i < MELODY_NOTE_COUNT; i++) {
        const melody_note_t note = melody_notes[i];
        parsed.hz = melody_note_hz(note);
        const char* error = check ? check_note(note, &parsed, reply) : play_note(note, parsed.volume_pct);
        if (error) {
            (void)snprintf(line, sizeof line, "MELODY_ERR %s", error);
            reply(line);
            return;
        }
    }
    if (check) {
        (void)snprintf(line, sizeof line, "MELODY_END notes=%zu", MELODY_NOTE_COUNT);
    } else {
        (void)snprintf(line, sizeof line, "MELODY_END notes=%zu ms=%" PRId64, MELODY_NOTE_COUNT,
                       (timing_now_us() - started) / MATH_US_PER_MS);
    }
    reply(line);
}
CONSOLE_VERB(melody, 0, console_verb_melody)
