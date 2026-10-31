/* Non-blocking playback of pack-owned sounds. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio/audio_out.h"
#include "sound/sound.h"

#define SOUND_VOICE_COUNT     8
#define SOUND_BUS_COUNT_MAX   8
#define SOUND_GAIN_MAX_PCT    AUDIO_OUT_VOLUME_MAX_PCT
#define SOUND_PRIORITY_NORMAL 128
#define SOUND_PRIORITY_MAX    UINT8_MAX
#define SOUND_BUS_SYSTEM      ((sound_bus_t){.id = 0})
#define SOUND_BUS_SFX         ((sound_bus_t){.id = 1})
#define SOUND_BUS_MUSIC       ((sound_bus_t){.id = 2})
#define SOUND_BUS_HAPTIC      ((sound_bus_t){.id = 3})
#define SOUND_BUS_MASTER      ((sound_bus_t){.id = SOUND_BUS_COUNT_MAX})
#define SOUND_BUS_INVALID     ((sound_bus_t){.id = UINT8_MAX})
#define SOUND_VOICE_NULL      ((sound_voice_t){0})

typedef struct {
    uint32_t id;
} sound_voice_t;

typedef struct {
    uint8_t id;
} sound_bus_t;

typedef struct {
    sound_bus_t bus;
    uint8_t gain_pct;
    uint8_t priority;
    bool loop;
} sound_play_opts_t;

/* Keep sound_t and its pack alive until app exit: use app state, never a stack local.
 * Stop, stealing and sound_is_playing() == false are not reclamation fences:
 * queued play and the current mix block may still read them. The shell's exit barrier waits before release.
 * A full table always steals the lowest-priority oldest voice, even for a lower-priority new play.
 * NULL options select SFX, full gain, normal priority and one-shot playback. */
sound_voice_t sound_play(const sound_t* sound, const sound_play_opts_t* opts);
void sound_stop(sound_voice_t voice);
void sound_set_gain(sound_voice_t voice, uint8_t pct);
bool sound_is_playing(sound_voice_t voice);
sound_bus_t sound_bus_create(const char* name);
void sound_bus_destroy(sound_bus_t bus);
void sound_set_bus_gain(sound_bus_t bus, uint8_t pct);
