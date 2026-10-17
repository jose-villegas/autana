/* Portable control and rendering state. Callers serialize control operations, never PCM decoding. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_tone.h"
#include "sound/sound_play.h"

#define SOUND_BUS_BUILTIN_COUNT 3
#define SOUND_BUS_NAME_MAX      24
#define SOUND_AMP_IDLE_MS       2000
#define SOUND_VOICE_GENERATIONS (UINT32_MAX / SOUND_VOICE_COUNT)
#define SOUND_GAIN_FACTOR_SCALE (SOUND_GAIN_MAX_PCT * SOUND_GAIN_MAX_PCT)

typedef enum {
    SOUND_COMMAND_CAPTURE_OPEN,
    SOUND_COMMAND_CAPTURE_CLOSE,
    SOUND_COMMAND_PLAY,
    SOUND_COMMAND_STOP,
    SOUND_COMMAND_GAIN,
    SOUND_COMMAND_BUS_GAIN,
    SOUND_COMMAND_BUS_CREATE,
    SOUND_COMMAND_BUS_DESTROY
} sound_command_kind_t;

typedef struct {
    sound_command_kind_t kind;
    uint32_t app_generation;
    sound_voice_t voice;
    sound_play_opts_t opts;
    sound_t sound;
    uint16_t tone_hz;
} sound_command_t;

typedef struct {
    sound_voice_t handle;
    uint64_t started;
    uint32_t app_generation;
    sound_play_opts_t opts;
    bool playing;
    bool audible;
} sound_control_voice_t;

typedef struct {
    char name[SOUND_BUS_NAME_MAX];
    uint8_t gain_pct;
    bool used;
} sound_bus_state_t;

typedef struct {
    sound_control_voice_t voices[SOUND_VOICE_COUNT];
    sound_bus_state_t buses[SOUND_BUS_COUNT_MAX];
    uint64_t starts;
    uint32_t app_generation;
    uint32_t dropped_commands;
    bool ready;
} sound_control_t;

typedef struct {
    sound_voice_t handle;
    sound_play_opts_t opts;
    uint32_t app_generation;
    sound_t sound;
    sound_cursor_t cursor;
    audio_tone_t tone;
    bool tone_source;
    bool playing;
} sound_mix_voice_t;

typedef enum { SOUND_AMP_OFF, SOUND_AMP_WARMING, SOUND_AMP_ON } sound_amp_state_t;

typedef struct {
    sound_mix_voice_t voices[SOUND_VOICE_COUNT];
    uint8_t bus_gain[SOUND_BUS_COUNT_MAX];
    uint8_t master_gain;
    sound_amp_state_t amp;
    uint64_t amp_changed_ms;
    uint64_t last_active_ms;
} sound_mixer_t;

void sound_control_init(sound_control_t* control, bool ready);
sound_voice_t sound_control_play(sound_control_t* control, const sound_t* sound, const sound_play_opts_t* opts,
                                 uint16_t tone_hz, sound_command_t* command);
bool sound_control_voice_command(sound_control_t* control, sound_voice_t voice, sound_command_kind_t kind, uint8_t gain,
                                 sound_command_t* command);
bool sound_control_is_playing(const sound_control_t* control, sound_voice_t voice);
bool sound_control_is_audible(const sound_control_t* control, sound_voice_t voice);
sound_bus_t sound_control_bus_create(sound_control_t* control, const char* name, sound_command_t* command);
bool sound_control_bus_command(sound_control_t* control, sound_bus_t bus, sound_command_kind_t kind, uint8_t gain,
                               sound_command_t* command);
void sound_control_exit(sound_control_t* control);
void sound_control_complete(sound_control_t* control, const sound_mixer_t* mixer);

void sound_mixer_init(sound_mixer_t* mixer);
void sound_mixer_apply(sound_mixer_t* mixer, const sound_command_t* command, uint32_t app_generation);
void sound_mixer_exit(sound_mixer_t* mixer, uint32_t app_generation);
void sound_mixer_render(sound_mixer_t* mixer, int16_t* out, int32_t* accumulator, int16_t* scratch, size_t frames,
                        uint64_t now_ms, uint32_t startup_ms);
