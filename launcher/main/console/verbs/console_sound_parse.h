/* Pack sound arguments share the asset name limit and percent-volume contract. */
#pragma once

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_pack.h"
#include "audio/audio_out.h"
#include "console/console_verbs.h"

#define CONSOLE_SOUND_WORD_COUNT_MAX 4

typedef enum { CONSOLE_SOUND_PLAY, CONSOLE_SOUND_STOP, CONSOLE_SOUND_STATS } console_sound_action_t;

typedef struct {
    console_sound_action_t action;
    char name[ASSET_NAME_MAX];
    uint32_t voice;
    uint8_t gain_pct;
    bool loop;
} console_sound_args_t;

static inline bool
console_sound_number(const char* text, uint32_t max, uint32_t* out) {
    if (!*text || strspn(text, "0123456789") != strlen(text)) {
        return false;
    }
    errno = 0;
    const unsigned long value = strtoul(text, NULL, 10);
    if (errno == ERANGE || value > max) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static inline bool
console_sound_parse(const char* args, console_sound_args_t* out) {
    char words[CONSOLE_SOUND_WORD_COUNT_MAX][ASSET_NAME_MAX];
    int count = 0;
    while (*args) {
        while (isspace((unsigned char)*args)) {
            args++;
        }
        if (!*args) {
            break;
        }
        if (count == CONSOLE_SOUND_WORD_COUNT_MAX) {
            return false;
        }
        size_t length = 0;
        while (args[length] && !isspace((unsigned char)args[length])) {
            length++;
        }
        if (length >= ASSET_NAME_MAX) {
            return false;
        }
        memcpy(words[count], args, length);
        words[count++][length] = '\0';
        args += length;
    }
    if (!count) {
        return false;
    }
    console_sound_args_t parsed = {.gain_pct = AUDIO_OUT_VOLUME_MAX_PCT};
    if (strcmp(words[0], "stats") == 0 && count == 1) {
        parsed.action = CONSOLE_SOUND_STATS;
    } else if (strcmp(words[0], "stop") == 0 && count <= 2) {
        parsed.action = CONSOLE_SOUND_STOP;
        if (count == 2 && strcmp(words[1], "all") != 0
            && (!console_sound_number(words[1], UINT32_MAX, &parsed.voice) || !parsed.voice)) {
            return false;
        }
    } else if (strcmp(words[0], "play") == 0 && count >= 2) {
        parsed.action = CONSOLE_SOUND_PLAY;
        if (strspn(words[1], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-")
            != strlen(words[1])) {
            return false;
        }
        (void)strcpy(parsed.name, words[1]);
        int next = 2;
        if (count > next && strcmp(words[next], "loop") == 0) {
            parsed.loop = true;
            next++;
        }
        if (count > next) {
            uint32_t gain;
            if (count != next + 1 || !console_sound_number(words[next], AUDIO_OUT_VOLUME_MAX_PCT, &gain)) {
                return false;
            }
            parsed.gain_pct = (uint8_t)gain;
        }
    } else {
        return false;
    }
    *out = parsed;
    return true;
}
