/* Frame-task ownership of console sound views and their mounted packs. */
#pragma once

#include <stdbool.h>

#include "asset/asset_pack.h"
#include "sound/sound.h"
#include "sound/sound_play.h"

#define CONSOLE_SOUND_LINE_MAX (sizeof("sound play ") + (2 * (ASSET_NAME_MAX - 1)) + sizeof("/ loop 100"))

typedef enum {
    CONSOLE_SOUND_AMP_ON,
    CONSOLE_SOUND_START,
    CONSOLE_SOUND_STOP_EVENT,
    CONSOLE_SOUND_STEAL,
    CONSOLE_SOUND_AMP_OFF,
    CONSOLE_SOUND_EVENT_COUNT
} console_sound_event_t;

typedef enum {
    CONSOLE_SOUND_PLAY,
    CONSOLE_SOUND_STOP,
    CONSOLE_SOUND_STATS,
    CONSOLE_SOUND_ONSET
} console_sound_action_t;

typedef struct {
    console_sound_action_t action;
    char name[ASSET_NAME_MAX];
    char pack[ASSET_NAME_MAX];
    uint32_t voice;
    uint16_t hz;
    console_sound_event_t event;
    uint8_t gain_pct;
    bool loop;
} console_sound_args_t;

/* Answers a play request taken from the frame mailbox. */
void console_sound_answer(const console_sound_args_t* args);
/* After sound_stop_app_voices(): releases all console views and pack references. */
void console_sound_app_exit(void);
/* Stops launcher playback and fences its reads before releasing views. */
void console_sound_app_enter(void);
void console_sound_track_voice(const sound_t* sound, sound_voice_t voice);

const sound_t* console_sound_view(const console_sound_args_t* args, int64_t* open_us);
