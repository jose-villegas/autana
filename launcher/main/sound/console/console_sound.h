/* Frame-task ownership of console sound views and their mounted packs. */
#pragma once

#include <stdbool.h>

#include "asset/asset_pack.h"

#define CONSOLE_SOUND_LINE_MAX (sizeof("sound play ") + (2 * (ASSET_NAME_MAX - 1)) + sizeof("/ loop 100"))

typedef enum {
    CONSOLE_SOUND_AMP_ON,
    CONSOLE_SOUND_START,
    CONSOLE_SOUND_STOP_EVENT,
    CONSOLE_SOUND_STEAL,
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

/* Stops tracked voices and fences borrowed reads before releasing mapped packs. */
void console_sound_release_views(void);
