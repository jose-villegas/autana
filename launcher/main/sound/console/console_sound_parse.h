/* Pack sound arguments share the asset name limit and percent-volume contract. */
#pragma once

#include <stdio.h>
#include <string.h>

#include "asset/asset_store.h"
#include "audio/audio_out.h"
#include "console/console_verbs.h"
#include "math/text/math_text.h"
#include "sound/console/console_sound.h"

#define CONSOLE_SOUND_WORD_COUNT_MAX 4
/* 500 Hz remains audible while the second difference rejects its steady tone. */
#define CONSOLE_SOUND_ONSET_HZ       500u
#define CONSOLE_SOUND_ONSET_GAIN     (AUDIO_OUT_VOLUME_MAX_PCT / 3u)

static const char* const console_sound_events[] = {"amp-on", "start", "stop", "steal", "amp-off"};

static inline bool
console_sound_number(const char* text, uint32_t max, uint32_t* out) {
    return mathu_parse(text, NULL, out) && *out <= max;
}

static inline bool
console_sound_parse(const char* args, console_sound_args_t* out) {
    char words[CONSOLE_SOUND_WORD_COUNT_MAX][CONSOLE_SOUND_LINE_MAX];
    char format[CONSOLE_SOUND_LINE_MAX];
    const size_t width = sizeof words[0] - 1;
    (void)snprintf(format, sizeof format, "%%%zus %%%zus %%%zus %%%zus %%c", width, width, width, width);
    char trailing;
    const int count = sscanf(args, format, words[0], words[1], words[2], words[3], &trailing);
    if (count <= 0 || count > CONSOLE_SOUND_WORD_COUNT_MAX) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (i != 1 && strlen(words[i]) >= ASSET_NAME_MAX) {
            return false;
        }
    }
    console_sound_args_t parsed = {.gain_pct = AUDIO_OUT_VOLUME_MAX_PCT};
    if (strcmp(words[0], "onset") == 0 && count >= 2) {
        parsed.action = CONSOLE_SOUND_ONSET;
        unsigned event = 0;
        while (event < CONSOLE_SOUND_EVENT_COUNT && strcmp(words[1], console_sound_events[event]) != 0) {
            event++;
        }
        uint32_t hz = CONSOLE_SOUND_ONSET_HZ;
        uint32_t gain = CONSOLE_SOUND_ONSET_GAIN;
        if (event == CONSOLE_SOUND_EVENT_COUNT
            || (count >= 3 && !console_sound_number(words[2], AUDIO_OUT_NYQUIST_HZ - 1, &hz))
            || (count >= 4 && !console_sound_number(words[3], AUDIO_OUT_VOLUME_MAX_PCT, &gain)) || !hz) {
            return false;
        }
        parsed.event = (console_sound_event_t)event;
        parsed.hz = (uint16_t)hz;
        parsed.gain_pct = (uint8_t)gain;
    } else if (strcmp(words[0], "stats") == 0 && count == 1) {
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
        const char* name = words[1];
        char* separator = strchr(words[1], '/');
        if (separator) {
            *separator = '\0';
            name = separator + 1;
            if (!*words[1] || strlen(words[1]) >= sizeof parsed.pack || strchr(name, '/')) {
                return false;
            }
            (void)strcpy(parsed.pack, words[1]);
        } else {
            (void)strcpy(parsed.pack, ASSET_ENGINE_PACK);
        }
        if (!*name || strlen(name) >= sizeof parsed.name) {
            return false;
        }
        (void)strcpy(parsed.name, name);
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
