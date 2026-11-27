/* Portable control and rendering state. Callers serialize control state; the renderer alone owns cursors and PCM decoding. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio/audio_tone.h"
#include "sound/sound_play.h"

#define SOUND_BUS_BUILTIN_COUNT     4
#define SOUND_BUS_NAME_MAX          24
#define SOUND_APP_GENERATION_NONE   0u
#define SOUND_APP_GENERATION_ENGINE 1u
#define SOUND_APP_GENERATION_FIRST  (SOUND_APP_GENERATION_ENGINE + 1u)
/* Exponential decay time constant (tau = converted frames), not a fade length.
 * The shortest decay that removed stop/steal clicks was half this; doubled for margin. */
#define SOUND_DECLICK_US            1000
#define SOUND_VOICE_GENERATIONS     (UINT32_MAX / SOUND_VOICE_COUNT)
#define SOUND_GAIN_SHIFT            15
#define SOUND_GAIN_ONE              (1u << SOUND_GAIN_SHIFT)
#define SOUND_GAIN_FACTOR_SCALE     (SOUND_GAIN_MAX_PCT * SOUND_GAIN_MAX_PCT)

typedef enum {
    SOUND_COMMAND_CAPTURE_PARK,
    SOUND_COMMAND_PLAY,
    SOUND_COMMAND_STOP,
    SOUND_COMMAND_GAIN,
    SOUND_COMMAND_BUS_GAIN,
    SOUND_COMMAND_BUS_CREATE,
    SOUND_COMMAND_BUS_DESTROY,
    SOUND_COMMAND_APP_EXIT
} sound_command_kind_t;

#define SOUND_COMMAND_KIND_BITS 4
_Static_assert(SOUND_COMMAND_APP_EXIT < (1u << SOUND_COMMAND_KIND_BITS), "command kind must fit the queue");

#define SOUND_CAPTURE_OK           1u
#define SOUND_CAPTURE_FAILED       0u
#define SOUND_CAPTURE_RESULT_BITS  1
#define SOUND_CAPTURE_SEQUENCE_MAX (UINT32_MAX >> SOUND_CAPTURE_RESULT_BITS)

typedef struct {
    uint32_t sequence;
    uint32_t requested;
    uint32_t parked;
} sound_capture_t;

typedef enum { SOUND_CAPTURE_WAIT, SOUND_CAPTURE_ACQUIRED, SOUND_CAPTURE_TIMED_OUT } sound_capture_acquire_result_t;

sound_capture_acquire_result_t sound_capture_acquire(sound_capture_t* capture, int64_t now_us, int64_t deadline_us,
                                                     uint32_t* sequence);
uint32_t sound_capture_begin(sound_capture_t* capture);
bool sound_capture_park(sound_capture_t* capture, uint32_t sequence);
bool sound_capture_is_parked(const sound_capture_t* capture, uint32_t sequence);
void sound_capture_resume(sound_capture_t* capture, uint32_t sequence);
uint32_t sound_capture_result(uint32_t sequence, bool ok);
bool sound_capture_matches(uint32_t result, uint32_t sequence);
bool sound_capture_succeeded(uint32_t result, uint32_t sequence);

typedef struct {
    sound_voice_t voice;
    uint32_t app_generation;

    union {
        const sound_t* sound;

        struct {
            uint16_t hz;
            uint16_t ms;
        } tone;

        void* waiter;
    };

    sound_bus_t bus;
    uint8_t gain_pct;
    uint8_t priority;
    uint8_t loop        : 1;
    uint8_t kind        : 4;
    uint8_t tone_source : 1;
} sound_command_t;

#define SOUND_COMMAND_BYTES 16
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(sound_command_t) == SOUND_COMMAND_BYTES, "queue command exceeds its RAM budget");
#else
_Static_assert(sizeof(sound_command_t) == SOUND_COMMAND_BYTES + sizeof(void*), "host queue layout");
#endif

typedef struct {
    uint64_t started;
    sound_voice_t handle;
    sound_play_opts_t opts;
    uint32_t owner;
    bool playing;
    bool audible;
} sound_control_voice_t;

typedef struct {
    uint32_t owner;
    char name[SOUND_BUS_NAME_MAX];
    uint8_t gain_pct;
    bool used;
} sound_bus_state_t;

typedef struct {
    sound_control_voice_t voices[SOUND_VOICE_COUNT];
    sound_bus_state_t buses[SOUND_BUS_COUNT_MAX];
    uint64_t starts;
    uint32_t app_generation;
    uint32_t next_generation;
    uint32_t dropped_commands;
    uint32_t read_generation;
    bool reset_gains;
    bool ready;
} sound_control_t;

typedef struct {
    sound_voice_t handle;

    union {
        sound_cursor_t cursor;

        struct {
            audio_tone_t oscillator;
            uint32_t frames_left;
        } tone;
    } source;

    sound_bus_t bus;
    uint8_t gain_pct;
    bool loop;
    bool tone_source;
    bool playing;
    bool first_render;
    int32_t last_sample;
} sound_mix_voice_t;

typedef enum { SOUND_AMP_WARMING, SOUND_AMP_ON } sound_amp_state_t;

typedef struct {
    sound_mix_voice_t voices[SOUND_VOICE_COUNT];
    uint8_t bus_gain[SOUND_BUS_COUNT_MAX];
    uint8_t master_gain;
    sound_amp_state_t amp;
    uint64_t amp_started_ms;
    uint32_t declick_frames;
    int32_t residual;
} sound_mixer_t;

void sound_control_init(sound_control_t* control, bool ready);
bool sound_control_admit(const sound_control_t* control, uint32_t generation);
sound_voice_t sound_control_play(sound_control_t* control, const sound_t* sound, const sound_play_opts_t* opts,
                                 uint16_t tone_hz, uint16_t tone_ms, sound_command_t* command, uint32_t generation);
bool sound_control_voice_command(sound_control_t* control, sound_voice_t voice, sound_command_kind_t kind, uint8_t gain,
                                 sound_command_t* command);
bool sound_control_is_playing(const sound_control_t* control, sound_voice_t voice);
bool sound_control_is_audible(const sound_control_t* control, sound_voice_t voice);
sound_bus_t sound_control_bus_create(sound_control_t* control, const char* name, sound_command_t* command,
                                     uint32_t generation);
bool sound_control_bus_command(sound_control_t* control, sound_bus_t bus, sound_command_kind_t kind, uint8_t gain,
                               sound_command_t* command);
void sound_control_begin_app(sound_control_t* control);
void sound_control_end_app(sound_control_t* control);
void sound_control_exit(sound_control_t* control);
uint32_t sound_control_begin_read(sound_control_t* control);
void sound_control_end_read(sound_control_t* control);
bool sound_control_read_pending(const sound_control_t* control, uint32_t generation);
void sound_control_complete(sound_control_t* control, const sound_mixer_t* mixer);

void sound_mixer_init(sound_mixer_t* mixer);
void sound_mixer_apply(sound_mixer_t* mixer, const sound_command_t* command, uint32_t app_generation);
void sound_mixer_sync(sound_mixer_t* mixer, const sound_control_t* control);
void sound_mixer_render(sound_mixer_t* mixer, int16_t* out, int32_t* accumulator, int16_t* scratch, size_t frames,
                        uint64_t now_ms, uint32_t startup_ms);
