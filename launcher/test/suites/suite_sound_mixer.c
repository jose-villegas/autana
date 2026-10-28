/* The control plane and PCM renderer run independently of an RTOS or codec. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_adpcm.h"
#include "fixtures/sound_mixer_oracle.h"
#include "math/scalar/rng.h"
#include "sound/sound_engine.h"
#include "sound/sound_mixer.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#define TEST_FRAMES     12
#define TEST_SAMPLE     16000
#define TEST_STARTUP_MS 250
#define TEST_HALF_GAIN  (SOUND_GAIN_MAX_PCT / 2)
#define TEST_TONE_HZ    1000

typedef struct {
    sound_control_t control;
    sound_mixer_t mixer;
    sound_t sound;
    sound_t extra_sound;
    uint8_t extra_pcm[SOUND_PCM_BYTES];
    uint8_t pcm[TEST_FRAMES * SOUND_PCM_BYTES];
    int16_t expected[TEST_FRAMES];
    int16_t out[TEST_FRAMES];
    int32_t accumulator[TEST_FRAMES];
    int16_t scratch[TEST_FRAMES];
} fixture_t;

static fixture_t* allocated;

static void
cleanup(void) {
    free(allocated);
    allocated = NULL;
}

static fixture_t*
fixture(void) {
    allocated = calloc(1, sizeof *allocated);
    TEST_ASSERT_NOT_NULL(allocated);
    suite_set_test_cleanup(cleanup);
    sound_control_init(&allocated->control, true);
    sound_control_begin_app(&allocated->control);
    sound_mixer_init(&allocated->mixer);
    allocated->sound = (sound_t){.data = allocated->pcm, .frames = TEST_FRAMES, .format = SOUND_PCM16};
    for (int i = 0; i < TEST_FRAMES; i++) {
        allocated->pcm[i * SOUND_PCM_BYTES] = (uint8_t)TEST_SAMPLE;
        allocated->pcm[i * SOUND_PCM_BYTES + 1] = (uint8_t)(TEST_SAMPLE >> CHAR_BIT);
    }
    return allocated;
}

static sound_voice_t
play(fixture_t* f, const sound_play_opts_t* opts) {
    sound_command_t command;
    const sound_voice_t voice =
        sound_control_play(&f->control, &f->sound, opts, 0, 0, &command, f->control.app_generation);
    TEST_ASSERT_NOT_EQUAL(0, voice.id);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    return voice;
}

static void
render(fixture_t* f, uint64_t now_ms, size_t frames) {
    sound_mixer_sync(&f->mixer, &f->control);
    sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, frames, now_ms, TEST_STARTUP_MS);
    sound_control_complete(&f->control, &f->mixer);
}

static void
test_sound_warmup_holds_positions_and_writes_silence(void) {
    fixture_t* f = fixture();
    const sound_voice_t voice = play(f, NULL);
    render(f, 0, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_WARMING, f->mixer.amp);
    for (int i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, f->out[i]);
    }
    render(f, TEST_STARTUP_MS - 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.cursor.position);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, voice));
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
    render(f, TEST_STARTUP_MS + SOUND_AMP_IDLE_MS - 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_ON, f->mixer.amp);
    render(f, TEST_STARTUP_MS + SOUND_AMP_IDLE_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_OFF, f->mixer.amp);
}

static void
test_sound_saturates_both_rails(void) {
    fixture_t* f = fixture();
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        (void)play(f, &opts);
    }
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, f->out[0]);
    for (int i = 0; i < TEST_FRAMES; i++) {
        const uint16_t sample = (uint16_t)(-TEST_SAMPLE);
        f->pcm[i * SOUND_PCM_BYTES] = (uint8_t)sample;
        f->pcm[i * SOUND_PCM_BYTES + 1] = (uint8_t)(sample >> CHAR_BIT);
    }
    render(f, TEST_STARTUP_MS + 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, f->out[0]);
}

static void
test_sound_voice_bus_and_master_gain(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_voice_t voice = play(f, NULL);
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_GAIN, TEST_HALF_GAIN, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    const sound_bus_t buses[] = {SOUND_BUS_SFX, SOUND_BUS_MASTER};
    for (size_t i = 0; i < sizeof buses / sizeof buses[0]; i++) {
        TEST_ASSERT_TRUE(
            sound_control_bus_command(&f->control, buses[i], SOUND_COMMAND_BUS_GAIN, TEST_HALF_GAIN, &command));
        sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    }
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    const int expected = TEST_SAMPLE * TEST_HALF_GAIN / SOUND_GAIN_MAX_PCT * TEST_HALF_GAIN / SOUND_GAIN_MAX_PCT
                         * TEST_HALF_GAIN / SOUND_GAIN_MAX_PCT;
    for (int i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(expected, f->out[i]);
    }
}

static void
test_sound_steals_lowest_priority_then_oldest(void) {
    fixture_t* f = fixture();
    sound_voice_t handles[SOUND_VOICE_COUNT];
    sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .priority = SOUND_PRIORITY_NORMAL};
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        opts.priority = (uint8_t)(SOUND_PRIORITY_NORMAL + SOUND_VOICE_COUNT - i);
        handles[i] = play(f, &opts);
    }
    const sound_voice_t stolen = play(f, NULL);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, handles[SOUND_VOICE_COUNT - 1]));
    for (int i = 0; i < SOUND_VOICE_COUNT - 1; i++) {
        TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, handles[i]));
    }
    sound_command_t command;
    TEST_ASSERT_FALSE(
        sound_control_voice_command(&f->control, handles[SOUND_VOICE_COUNT - 1], SOUND_COMMAND_STOP, 0, &command));
    TEST_ASSERT_FALSE(
        sound_control_voice_command(&f->control, handles[SOUND_VOICE_COUNT - 1], SOUND_COMMAND_GAIN, 0, &command));
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, stolen));
    sound_control_exit(&f->control);
    sound_control_begin_app(&f->control);
    sound_mixer_sync(&f->mixer, &f->control);
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        handles[i] = play(f, NULL);
    }
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        const sound_voice_t next = play(f, NULL);
        TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, handles[i]));
        TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, next));
    }
}

static void
test_sound_stop_and_completion_do_not_resurrect_handles(void) {
    fixture_t* f = fixture();
    sound_voice_t voice = play(f, NULL);
    sound_command_t command;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_STOP, 0, &command));
    sound_control_complete(&f->control, &f->mixer);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_FALSE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_GAIN, TEST_HALF_GAIN, &command));
    const sound_voice_t next = play(f, NULL);
    TEST_ASSERT_NOT_EQUAL(voice.id, next.id);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
}

static void
test_sound_bus_capacity_destroy_and_reuse(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    sound_bus_t buses[SOUND_BUS_COUNT_MAX];
    for (int i = SOUND_BUS_BUILTIN_COUNT; i < SOUND_BUS_COUNT_MAX; i++) {
        buses[i] = sound_control_bus_create(&f->control, "test", &command, f->control.app_generation);
        TEST_ASSERT_EQUAL_INT(i, buses[i].id);
        sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    }
    TEST_ASSERT_EQUAL_INT(SOUND_BUS_INVALID.id,
                          sound_control_bus_create(&f->control, "full", &command, f->control.app_generation).id);
    const sound_bus_t bus = buses[SOUND_BUS_BUILTIN_COUNT];
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = SOUND_GAIN_MAX_PCT};
    const sound_voice_t voice = play(f, &opts);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
    TEST_ASSERT_EQUAL_INT(bus.id,
                          sound_control_bus_create(&f->control, "reuse", &command, f->control.app_generation).id);
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, SOUND_BUS_SYSTEM, SOUND_COMMAND_BUS_DESTROY, 0, &command));
}

static void
test_sound_exit_drops_queued_app_commands_preserves_system(void) {
    fixture_t* f = fixture();
    sound_command_t app_command, system_command;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT};
    sound_control_end_app(&f->control);
    const sound_voice_t system =
        sound_control_play(&f->control, &f->sound, &opts, 0, 0, &system_command, f->control.app_generation);
    sound_control_begin_app(&f->control);
    const sound_voice_t app =
        sound_control_play(&f->control, &f->sound, NULL, 0, 0, &app_command, f->control.app_generation);
    const sound_bus_t bus =
        sound_control_bus_create(&f->control, "app", &(sound_command_t){0}, f->control.app_generation);
    TEST_ASSERT_TRUE(bus.id < SOUND_BUS_COUNT_MAX);
    sound_control_exit(&f->control);
    sound_mixer_sync(&f->mixer, &f->control);
    TEST_ASSERT_NOT_EQUAL(0, app.id);
    TEST_ASSERT_NOT_EQUAL(0, system.id);
    sound_mixer_apply(&f->mixer, &app_command, f->control.app_generation);
    TEST_ASSERT_FALSE(f->mixer.voices[(app.id - 1) % SOUND_VOICE_COUNT].playing);
    sound_mixer_apply(&f->mixer, &system_command, f->control.app_generation);
    TEST_ASSERT_TRUE(f->mixer.voices[(system.id - 1) % SOUND_VOICE_COUNT].playing);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, app));
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, system));
    TEST_ASSERT_FALSE(f->control.buses[bus.id].used);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
}

static void
test_sound_loops_seek_and_honors_authored_loop_end(void) {
    fixture_t* f = fixture();
    f->sound.frames = 1;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    const sound_voice_t loop = play(f, &opts);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, loop));
    for (int i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[i]);
    }
    sound_control_exit(&f->control);
    sound_control_begin_app(&f->control);
    sound_mixer_sync(&f->mixer, &f->control);
    f->sound.frames = TEST_FRAMES;
    f->sound.loop_start = 1;
    f->sound.loop_end = TEST_FRAMES / 2;
    const sound_voice_t once = play(f, NULL);
    render(f, TEST_STARTUP_MS + 1, TEST_FRAMES);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, once));
    for (int i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(i < TEST_FRAMES / 2 ? TEST_SAMPLE : 0, f->out[i]);
    }
}

static void
test_sound_failed_init_is_noop(void) {
    fixture_t* f = fixture();
    sound_control_init(&f->control, false);
    sound_command_t command;
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, NULL, 0, 0, &command, f->control.app_generation).id);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, SOUND_VOICE_NULL));
    TEST_ASSERT_FALSE(
        sound_control_voice_command(&f->control, (sound_voice_t){.id = 1}, SOUND_COMMAND_STOP, 0, &command));
    TEST_ASSERT_EQUAL_INT(SOUND_BUS_INVALID.id,
                          sound_control_bus_create(&f->control, "no", &command, f->control.app_generation).id);
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, SOUND_BUS_SFX, SOUND_COMMAND_BUS_GAIN, 0, &command));
    const uint32_t generation = f->control.app_generation;
    sound_control_exit(&f->control);
    TEST_ASSERT_EQUAL_UINT32(generation, f->control.app_generation);
}

static sound_voice_t
play_tone(fixture_t* f, uint16_t ms, sound_bus_t bus) {
    sound_command_t command;
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = SOUND_GAIN_MAX_PCT};
    const sound_voice_t voice =
        sound_control_play(&f->control, NULL, &opts, TEST_TONE_HZ, ms, &command, f->control.app_generation);
    TEST_ASSERT_NOT_EQUAL(0, voice.id);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    render(f, 0, TEST_FRAMES);
    return voice;
}

static void
test_sound_tone_source_holds_phase_until_warm(void) {
    fixture_t* f = fixture();
    const sound_voice_t voice = play_tone(f, SOUND_TONE_UNTIL_STOPPED, SOUND_BUS_SYSTEM);
    render(f, TEST_STARTUP_MS - 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.tone.oscillator.phase);
    TEST_ASSERT_FALSE(sound_control_is_audible(&f->control, voice));
    audio_tone_t expected;
    audio_tone_init(&expected, TEST_TONE_HZ);
    audio_tone_fill(&expected, f->expected, TEST_FRAMES, AUDIO_OUT_VOLUME_MAX_PCT);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16_ARRAY(f->expected, f->out, TEST_FRAMES);
    TEST_ASSERT_TRUE(sound_control_is_audible(&f->control, voice));
}

#define TEST_TONE_MS     7u
#define TEST_TONE_FRAMES audio_out_samples(TEST_TONE_MS)

static void
test_sound_timed_tone_ends_on_time(void) {
    fixture_t* f = fixture();
    const uint16_t durations[] = {1, TEST_TONE_MS, TEST_TONE_MS * TEST_FRAMES};
    for (size_t i = 0; i < sizeof durations / sizeof durations[0]; i++) {
        const sound_voice_t voice = play_tone(f, durations[i], SOUND_BUS_SYSTEM);
        const size_t frames = audio_out_samples(durations[i]);
        audio_tone_t expected;
        audio_tone_init(&expected, TEST_TONE_HZ);
        size_t rendered = 0;
        while (rendered < frames) {
            TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, voice));
            const size_t count = frames - rendered < TEST_FRAMES ? frames - rendered : TEST_FRAMES;
            audio_tone_fill(&expected, f->expected, count, AUDIO_OUT_VOLUME_MAX_PCT);
            render(f, TEST_STARTUP_MS, TEST_FRAMES);
            TEST_ASSERT_EQUAL_INT16_ARRAY(f->expected, f->out, count);
            for (size_t j = count; j < TEST_FRAMES; j++) {
                TEST_ASSERT_EQUAL_INT16(0, f->out[j]);
            }
            rendered += count;
        }
        TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
        TEST_ASSERT_FALSE(sound_control_is_audible(&f->control, voice));
        f->mixer.amp = SOUND_AMP_OFF;
    }
}

static void
test_sound_tone_uses_requested_bus_gain_and_mute(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    TEST_ASSERT_TRUE(
        sound_control_bus_command(&f->control, SOUND_BUS_MUSIC, SOUND_COMMAND_BUS_GAIN, TEST_HALF_GAIN, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    const sound_voice_t voice = play_tone(f, TEST_TONE_MS, SOUND_BUS_MUSIC);
    audio_tone_t expected;
    audio_tone_init(&expected, TEST_TONE_HZ);
    audio_tone_fill(&expected, f->expected, TEST_FRAMES, AUDIO_OUT_VOLUME_MAX_PCT);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    const uint32_t factor = TEST_HALF_GAIN * SOUND_GAIN_ONE / SOUND_GAIN_MAX_PCT;
    for (size_t i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16((f->expected[i] * (int32_t)factor) >> SOUND_GAIN_SHIFT, f->out[i]);
    }
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, SOUND_BUS_MUSIC, SOUND_COMMAND_BUS_GAIN, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    for (size_t i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, f->out[i]);
    }
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, voice));
}

static void
test_sound_tone_until_stopped(void) {
    fixture_t* f = fixture();
    const sound_voice_t voice = play_tone(f, SOUND_TONE_UNTIL_STOPPED, SOUND_BUS_SYSTEM);
    for (size_t i = 0; i <= TEST_TONE_FRAMES; i++) {
        render(f, TEST_STARTUP_MS, TEST_FRAMES);
        TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, voice));
    }
    sound_command_t command;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_STOP, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
    for (size_t i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, f->out[i]);
    }
}

static void
test_sound_borrows_view_and_retires_a_stopped_queued_steal(void) {
    fixture_t* f = fixture();
    const sound_voice_t old = play(f, NULL);
    const int slot = (int)((old.id - 1) % SOUND_VOICE_COUNT);
    TEST_ASSERT_EQUAL_PTR(&f->sound, f->mixer.voices[slot].source.cursor.sound);
    for (int i = 1; i < SOUND_VOICE_COUNT; i++) {
        (void)play(f, NULL);
    }
    sound_command_t pending;
    const sound_voice_t next =
        sound_control_play(&f->control, &f->sound, NULL, 0, 0, &pending, f->control.app_generation);
    TEST_ASSERT_NOT_EQUAL(0, next.id);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, old));
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, next, SOUND_COMMAND_STOP, 0, &pending));
    sound_mixer_apply(&f->mixer, &pending, f->control.app_generation);
    sound_mixer_sync(&f->mixer, &f->control);
    TEST_ASSERT_FALSE(f->mixer.voices[slot].playing);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, next));
}

static void
test_sound_exit_waits_only_for_the_retired_read_epoch(void) {
    fixture_t* f = fixture();
    const uint32_t old = sound_control_begin_read(&f->control);
    TEST_ASSERT_EQUAL_UINT32(f->control.app_generation, old);
    TEST_ASSERT_TRUE(sound_control_read_pending(&f->control, old));
    sound_control_exit(&f->control);
    TEST_ASSERT_TRUE(sound_control_read_pending(&f->control, old));
    TEST_ASSERT_FALSE(sound_control_read_pending(&f->control, f->control.app_generation));
    sound_control_end_read(&f->control);
    TEST_ASSERT_FALSE(sound_control_read_pending(&f->control, old));
    sound_control_begin_app(&f->control);
    const uint32_t next = sound_control_begin_read(&f->control);
    TEST_ASSERT_FALSE(sound_control_read_pending(&f->control, old));
    TEST_ASSERT_TRUE(sound_control_read_pending(&f->control, next));
    sound_control_end_read(&f->control);
    f->control.app_generation = UINT32_MAX;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, sound_control_begin_read(&f->control));
    sound_control_exit(&f->control);
    TEST_ASSERT_EQUAL_UINT32(SOUND_APP_GENERATION_NONE, f->control.app_generation);
    TEST_ASSERT_TRUE(sound_control_read_pending(&f->control, UINT32_MAX));
    sound_control_end_read(&f->control);
    sound_control_init(&f->control, false);
    TEST_ASSERT_EQUAL_UINT32(SOUND_APP_GENERATION_NONE, sound_control_begin_read(&f->control));
    TEST_ASSERT_FALSE(sound_control_read_pending(&f->control, SOUND_APP_GENERATION_NONE));
}

static void
test_sound_exit_closes_play_and_stops_app_system(void) {
    fixture_t* f = fixture();
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT};
    const sound_voice_t voice = play(f, &opts);
    sound_control_exit(&f->control);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
    TEST_ASSERT_EQUAL_UINT32(SOUND_APP_GENERATION_NONE, f->control.app_generation);
    sound_command_t command;
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, &opts, 0, 0, &command, f->control.app_generation).id);
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, NULL, 0, 0, &command, f->control.app_generation).id);
}

static void
set_sample(fixture_t* f, int16_t value) {
    for (int i = 0; i < TEST_FRAMES; i++) {
        f->pcm[i * SOUND_PCM_BYTES] = (uint8_t)value;
        f->pcm[i * SOUND_PCM_BYTES + 1] = (uint8_t)((uint16_t)value >> CHAR_BIT);
    }
}

static void
test_sound_gain_sweep_uses_each_stage_and_bus(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_bus_t created = sound_control_bus_create(&f->control, "gain", &command, f->control.app_generation);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    const sound_bus_t buses[] = {SOUND_BUS_SYSTEM, SOUND_BUS_MUSIC, created};
    for (size_t b = 0; b < sizeof buses / sizeof buses[0]; b++) {
        for (unsigned gain = 0; gain <= SOUND_GAIN_MAX_PCT; gain++) {
            for (int sign = -1; sign <= 1; sign += 2) {
                set_sample(f, (int16_t)(sign * TEST_SAMPLE));
                const sound_play_opts_t opts = {.bus = buses[b], .gain_pct = (uint8_t)gain};
                (void)play(f, &opts);
                const uint8_t bus_gain = (uint8_t)(SOUND_GAIN_MAX_PCT / (b + 2));
                const uint8_t master_gain = SOUND_GAIN_MAX_PCT * 3 / 4;
                TEST_ASSERT_TRUE(
                    sound_control_bus_command(&f->control, buses[b], SOUND_COMMAND_BUS_GAIN, bus_gain, &command));
                sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
                TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, SOUND_BUS_MASTER, SOUND_COMMAND_BUS_GAIN,
                                                           master_gain, &command));
                sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
                sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, TEST_FRAMES, 0, 0);
                sound_control_complete(&f->control, &f->mixer);
                const int exact = (int)((int64_t)sign * TEST_SAMPLE * gain * bus_gain * master_gain
                                        / (SOUND_GAIN_FACTOR_SCALE * SOUND_GAIN_MAX_PCT));
                TEST_ASSERT_INT_WITHIN(1, exact, f->out[0]);
            }
        }
    }
}

static void
test_sound_bus_reuse_resets_render_gain(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_bus_t bus = sound_control_bus_create(&f->control, "first", &command, f->control.app_generation);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_GAIN, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_EQUAL_UINT8(bus.id,
                            sound_control_bus_create(&f->control, "next", &command, f->control.app_generation).id);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = SOUND_GAIN_MAX_PCT};
    (void)play(f, &opts);
    sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, TEST_FRAMES, 0, 0);
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
}

static void
test_sound_delayed_controls_ignore_reused_slot(void) {
    fixture_t* f = fixture();
    const sound_voice_t old = play(f, NULL);
    sound_command_t stop, gain;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, old, SOUND_COMMAND_GAIN, 0, &gain));
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, old, SOUND_COMMAND_STOP, 0, &stop));
    const sound_voice_t next = play(f, NULL);
    sound_mixer_apply(&f->mixer, &stop, f->control.app_generation);
    sound_mixer_apply(&f->mixer, &gain, f->control.app_generation);
    TEST_ASSERT_TRUE(f->mixer.voices[0].playing);
    TEST_ASSERT_EQUAL_UINT32(next.id, f->mixer.voices[0].handle.id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.voices[0].gain_pct);
}

static void
test_sound_exit_never_reads_retired_source_and_preserves_engine(void) {
    fixture_t* f = fixture();
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    (void)play(f, &opts);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, 1);
    sound_control_exit(&f->control);
    sound_mixer_sync(&f->mixer, &f->control);
    f->sound.data = NULL;
    render(f, TEST_STARTUP_MS + 1, TEST_FRAMES);
    for (int i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, f->out[i]);
    }
    f->sound.data = f->pcm;
    sound_control_end_app(&f->control);
    const sound_voice_t engine = play(f, &opts);
    sound_control_begin_app(&f->control);
    const sound_bus_t bus =
        sound_control_bus_create(&f->control, "owned", &(sound_command_t){0}, f->control.app_generation);
    sound_control_exit(&f->control);
    render(f, TEST_STARTUP_MS + 2, TEST_FRAMES);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, engine));
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
    TEST_ASSERT_FALSE(f->control.buses[bus.id].used);
    sound_control_end_app(&f->control);
    sound_control_begin_app(&f->control);
    TEST_ASSERT_NOT_EQUAL(SOUND_APP_GENERATION_NONE, play(f, NULL).id);
}

static void
test_sound_amp_rewarms_and_idle_window_refreshes(void) {
    fixture_t* f = fixture();
    (void)play(f, NULL);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    (void)play(f, NULL);
    const uint64_t inside = TEST_STARTUP_MS + SOUND_AMP_IDLE_MS - 1;
    render(f, inside, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
    render(f, inside + SOUND_AMP_IDLE_MS - 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_ON, f->mixer.amp);
    const uint64_t late = inside + SOUND_AMP_IDLE_MS * SOUND_AMP_IDLE_MS;
    render(f, late, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_OFF, f->mixer.amp);
    (void)play(f, NULL);
    render(f, late + 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_WARMING, f->mixer.amp);
    TEST_ASSERT_EQUAL_INT16(0, f->out[0]);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.cursor.position);
    render(f, late + TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.cursor.position);
    render(f, late + TEST_STARTUP_MS + 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
}

static void
test_sound_free_slot_precedes_steal_and_handles_wrap(void) {
    fixture_t* f = fixture();
    const sound_voice_t first = play(f, NULL);
    sound_command_t command;
    sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .priority = SOUND_PRIORITY_MAX};
    const sound_voice_t second = play(f, &opts);
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, second, SOUND_COMMAND_STOP, 0, &command));
    opts.priority = 0;
    const sound_voice_t next = play(f, &opts);
    TEST_ASSERT_EQUAL_UINT32((second.id - 1) % SOUND_VOICE_COUNT, (next.id - 1) % SOUND_VOICE_COUNT);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, first));
    sound_control_init(&f->control, true);
    for (int slot = 0; slot < SOUND_VOICE_COUNT; slot++) {
        f->control.voices[slot].handle.id = (SOUND_VOICE_GENERATIONS - 1) * SOUND_VOICE_COUNT + (uint32_t)slot + 1;
    }
    sound_voice_t handles[SOUND_VOICE_COUNT];
    for (int slot = 0; slot < SOUND_VOICE_COUNT; slot++) {
        handles[slot] = sound_control_play(&f->control, &f->sound, NULL, 0, 0, &command, f->control.app_generation);
        TEST_ASSERT_NOT_EQUAL(0, handles[slot].id);
        for (int previous = 0; previous < slot; previous++) {
            TEST_ASSERT_NOT_EQUAL(handles[previous].id, handles[slot].id);
            TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, handles[previous]));
        }
    }
}

static void
test_sound_invalid_arguments_leave_control_and_command_unchanged(void) {
    fixture_t* f = fixture();
    sound_command_t command = {.kind = SOUND_COMMAND_CAPTURE_PARK};
    const sound_command_t before = command;
    const sound_control_t saved = f->control;
    sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT + 1};
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, &opts, 0, 0, &command, f->control.app_generation).id);
    opts.gain_pct = SOUND_GAIN_MAX_PCT;
    opts.bus = SOUND_BUS_INVALID;
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, &opts, 0, 0, &command, f->control.app_generation).id);
    TEST_ASSERT_EQUAL_UINT32(0,
                             sound_control_play(&f->control, NULL, NULL, 0, 0, &command, f->control.app_generation).id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_INVALID.id,
                            sound_control_bus_create(&f->control, "", &command, f->control.app_generation).id);
    char name[SOUND_BUS_NAME_MAX + 1];
    memset(name, 'x', sizeof name);
    name[sizeof name - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_INVALID.id,
                            sound_control_bus_create(&f->control, name, &command, f->control.app_generation).id);
    TEST_ASSERT_EQUAL_MEMORY(&saved, &f->control, sizeof saved);
    TEST_ASSERT_EQUAL_MEMORY(&before, &command, sizeof before);
    name[SOUND_BUS_NAME_MAX - 1] = '\0';
    const sound_bus_t bus = sound_control_bus_create(&f->control, name, &command, f->control.app_generation);
    TEST_ASSERT_NOT_EQUAL(SOUND_BUS_INVALID.id, bus.id);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_GAIN, 0, &command));
    opts.bus = bus;
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, &opts, 0, 0, &command, f->control.app_generation).id);
}

static void
test_sound_authored_loop_crosses_blocks_and_empty_read_stops(void) {
    fixture_t* f = fixture();
    f->sound.loop_start = 1;
    f->sound.loop_end = TEST_FRAMES / 2;
    for (int i = 0; i < TEST_FRAMES; i++) {
        f->pcm[i * SOUND_PCM_BYTES] = (uint8_t)i;
        f->pcm[i * SOUND_PCM_BYTES + 1] = 0;
    }
    const sound_play_opts_t opts = {.bus = SOUND_BUS_MUSIC, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    (void)play(f, &opts);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS / 2, TEST_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.cursor.position);
    for (int block = 0; block < TEST_FRAMES; block++) {
        render(f, TEST_STARTUP_MS + (uint32_t)block, TEST_FRAMES);
        for (int i = 0; i < TEST_FRAMES; i++) {
            const int position = block * TEST_FRAMES + i;
            const int expected =
                position < (int)f->sound.loop_end ? position : 1 + (position - 1) % ((int)f->sound.loop_end - 1);
            TEST_ASSERT_EQUAL_INT16(expected, f->out[i]);
        }
    }
    f->sound.loop_end = 0;
    f->sound.frames = f->mixer.voices[0].source.cursor.position;
    render(f, TEST_STARTUP_MS + TEST_FRAMES, TEST_FRAMES);
    TEST_ASSERT_FALSE(f->mixer.voices[0].playing);
}

static void
test_sound_saturation_exact_rails_and_one_over(void) {
    fixture_t* f = fixture();
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    for (int sign = -1; sign <= 1; sign += 2) {
        sound_control_exit(&f->control);
        sound_control_begin_app(&f->control);
        sound_mixer_sync(&f->mixer, &f->control);
        const int16_t rail = sign < 0 ? INT16_MIN : INT16_MAX;
        set_sample(f, rail);
        (void)play(f, &opts);
        sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, TEST_FRAMES, 0, 0);
        TEST_ASSERT_EQUAL_INT16(rail, f->out[0]);
        f->extra_pcm[0] = (uint8_t)sign;
        f->extra_pcm[1] = (uint8_t)((uint16_t)sign >> CHAR_BIT);
        f->extra_sound = (sound_t){.data = f->extra_pcm, .frames = 1, .format = SOUND_PCM16};
        sound_command_t command;
        TEST_ASSERT_NOT_EQUAL(
            0, sound_control_play(&f->control, &f->extra_sound, &opts, 0, 0, &command, f->control.app_generation).id);
        sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
        sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, TEST_FRAMES, 0, 0);
        TEST_ASSERT_EQUAL_INT32((int32_t)rail + sign, f->accumulator[0]);
        TEST_ASSERT_EQUAL_INT16(rail, f->out[0]);
    }
}

static void
test_sound_command_queue_round_trip(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_play_opts_t opts = {
        .bus = SOUND_BUS_MUSIC, .gain_pct = TEST_HALF_GAIN, .priority = SOUND_PRIORITY_MAX, .loop = true};
    (void)sound_control_play(&f->control, NULL, &opts, TEST_TONE_HZ, TEST_TONE_MS, &command, f->control.app_generation);
    uint8_t queued[sizeof command];
    memcpy(queued, &command, sizeof command);
    sound_command_t restored;
    memcpy(&restored, queued, sizeof restored);
    TEST_ASSERT_EQUAL_INT(SOUND_COMMAND_PLAY, restored.kind);
    TEST_ASSERT_TRUE(restored.loop);
    TEST_ASSERT_TRUE(restored.tone_source);
    TEST_ASSERT_EQUAL_UINT16(TEST_TONE_HZ, restored.tone.hz);
    TEST_ASSERT_EQUAL_UINT16(TEST_TONE_MS, restored.tone.ms);
    TEST_ASSERT_EQUAL_UINT8(SOUND_PRIORITY_MAX, restored.priority);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_MUSIC.id, restored.bus.id);
    TEST_ASSERT_EQUAL_UINT32(f->control.app_generation, restored.app_generation);
    sound_mixer_apply(&f->mixer, &restored, f->control.app_generation);
    TEST_ASSERT_TRUE(f->mixer.voices[0].tone_source);
}

static void
test_sound_exit_resets_app_gains_and_preserves_system_gain(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_bus_t buses[] = {SOUND_BUS_SYSTEM, SOUND_BUS_SFX, SOUND_BUS_MUSIC, SOUND_BUS_MASTER};
    for (size_t i = 0; i < sizeof buses / sizeof buses[0]; i++) {
        TEST_ASSERT_TRUE(
            sound_control_bus_command(&f->control, buses[i], SOUND_COMMAND_BUS_GAIN, TEST_HALF_GAIN, &command));
        sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    }
    sound_control_exit(&f->control);
    TEST_ASSERT_TRUE(f->control.reset_gains);
    command = (sound_command_t){.kind = SOUND_COMMAND_APP_EXIT};
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_EQUAL_UINT8(TEST_HALF_GAIN, f->mixer.bus_gain[SOUND_BUS_SYSTEM.id]);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.bus_gain[SOUND_BUS_SFX.id]);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.bus_gain[SOUND_BUS_MUSIC.id]);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.master_gain);
    f->control.reset_gains = false;
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, SOUND_BUS_SFX, SOUND_COMMAND_BUS_GAIN, 0, &command));
    sound_control_end_app(&f->control);
    TEST_ASSERT_TRUE(f->control.reset_gains);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->control.buses[SOUND_BUS_SFX.id].gain_pct);
    command = (sound_command_t){.kind = SOUND_COMMAND_APP_EXIT};
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.bus_gain[SOUND_BUS_SFX.id]);
}

static void
test_sound_exit_window_rejects_controls_and_none_commands(void) {
    fixture_t* f = fixture();
    sound_control_end_app(&f->control);
    const sound_voice_t engine = play(f, NULL);
    sound_control_begin_app(&f->control);
    sound_control_exit(&f->control);
    sound_command_t command;
    TEST_ASSERT_FALSE(sound_control_voice_command(&f->control, engine, SOUND_COMMAND_STOP, 0, &command));
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, engine));
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, SOUND_BUS_SYSTEM, SOUND_COMMAND_BUS_GAIN, 0, &command));
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->control.buses[SOUND_BUS_SYSTEM.id].gain_pct);
    command = (sound_command_t){.kind = SOUND_COMMAND_BUS_GAIN, .bus = SOUND_BUS_SYSTEM};
    sound_mixer_apply(&f->mixer, &command, SOUND_APP_GENERATION_NONE);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.bus_gain[SOUND_BUS_SYSTEM.id]);
    command = (sound_command_t){.kind = SOUND_COMMAND_STOP, .voice = engine};
    sound_mixer_apply(&f->mixer, &command, SOUND_APP_GENERATION_NONE);
    TEST_ASSERT_TRUE(f->mixer.voices[(engine.id - 1) % SOUND_VOICE_COUNT].playing);
}

static void
test_sound_producer_admission_cannot_cross_teardown(void) {
    fixture_t* f = fixture();
    const uint32_t admitted = f->control.app_generation;
    TEST_ASSERT_TRUE(sound_control_admit(&f->control, admitted));
    sound_control_exit(&f->control);
    TEST_ASSERT_FALSE(sound_control_admit(&f->control, admitted));
    sound_control_end_app(&f->control);
    TEST_ASSERT_FALSE(sound_control_admit(&f->control, admitted));
    sound_command_t command;
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, NULL, 0, 0, &command, admitted).id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_INVALID.id,
                            sound_control_bus_create(&f->control, "retired", &command, admitted).id);
    sound_control_begin_app(&f->control);
    TEST_ASSERT_FALSE(sound_control_admit(&f->control, admitted));
    TEST_ASSERT_TRUE(sound_control_admit(&f->control, f->control.app_generation));
    f->control.ready = false;
    TEST_ASSERT_FALSE(sound_control_admit(&f->control, f->control.app_generation));
}

static void
test_sound_stale_bus_commands_cannot_change_next_session(void) {
    fixture_t* f = fixture();
    const uint32_t retired = f->control.app_generation;
    sound_control_exit(&f->control);
    sound_control_begin_app(&f->control);
    sound_command_t command;
    const sound_bus_t bus = sound_control_bus_create(&f->control, "current", &command, f->control.app_generation);
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    const sound_voice_t voice = play(f, &opts);
    const int slot = (int)((voice.id - 1) % SOUND_VOICE_COUNT);
    const sound_command_kind_t kinds[] = {SOUND_COMMAND_BUS_GAIN, SOUND_COMMAND_BUS_CREATE, SOUND_COMMAND_BUS_DESTROY};
    const uint32_t owners[] = {retired, SOUND_APP_GENERATION_ENGINE, f->control.app_generation};
    for (size_t owner = 0; owner < sizeof owners / sizeof owners[0]; owner++) {
        for (size_t kind = 0; kind < sizeof kinds / sizeof kinds[0]; kind++) {
            f->mixer.bus_gain[bus.id] = TEST_HALF_GAIN;
            f->mixer.voices[slot].playing = true;
            command = (sound_command_t){.kind = kinds[kind], .app_generation = owners[owner], .bus = bus};
            sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
            const bool accepted = owners[owner] != retired;
            const int gain = !accepted || kinds[kind] == SOUND_COMMAND_BUS_DESTROY ? TEST_HALF_GAIN
                             : kinds[kind] == SOUND_COMMAND_BUS_CREATE             ? SOUND_GAIN_MAX_PCT
                                                                                   : 0;
            TEST_ASSERT_EQUAL_UINT8(gain, f->mixer.bus_gain[bus.id]);
            TEST_ASSERT_EQUAL_INT(!accepted || kinds[kind] != SOUND_COMMAND_BUS_DESTROY, f->mixer.voices[slot].playing);
        }
    }
}

static sound_voice_t
reserve_replacement(fixture_t* f, sound_command_t* command) {
    const sound_voice_t old = play(f, NULL);
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, old, SOUND_COMMAND_STOP, 0, command));
    const sound_voice_t next =
        sound_control_play(&f->control, &f->sound, NULL, 0, 0, command, f->control.app_generation);
    TEST_ASSERT_NOT_EQUAL(old.id, next.id);
    return next;
}

static void
test_sound_completion_preserves_a_queued_replacement(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_voice_t next = reserve_replacement(f, &command);
    f->mixer.voices[0].playing = false;
    sound_control_complete(&f->control, &f->mixer);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, next));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    render(f, 0, 1);
    render(f, TEST_STARTUP_MS, 1);
    TEST_ASSERT_TRUE(sound_control_is_audible(&f->control, next));
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[0]);
}

static void
test_sound_sync_stops_an_unreplaced_renderer_handle(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    (void)reserve_replacement(f, &command);
    sound_mixer_sync(&f->mixer, &f->control);
    TEST_ASSERT_FALSE(f->mixer.voices[0].playing);
}

static void
test_sound_none_generation_never_has_a_pending_read(void) {
    fixture_t* f = fixture();
    TEST_ASSERT_FALSE(sound_control_read_pending(&f->control, SOUND_APP_GENERATION_NONE));
    (void)sound_control_begin_read(&f->control);
    sound_control_end_read(&f->control);
    TEST_ASSERT_FALSE(sound_control_read_pending(&f->control, SOUND_APP_GENERATION_NONE));
}

static void
test_sound_exit_preserves_engine_owners_and_retires_only_app_owners(void) {
    fixture_t* f = fixture();
    sound_control_end_app(&f->control);
    const sound_voice_t engine = play(f, NULL);
    sound_command_t command;
    const sound_bus_t engine_bus = sound_control_bus_create(&f->control, "engine", &command, f->control.app_generation);
    sound_control_exit(&f->control);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, engine));
    TEST_ASSERT_TRUE(f->control.buses[engine_bus.id].used);
    sound_control_begin_app(&f->control);
    const sound_voice_t app = play(f, NULL);
    const sound_bus_t app_bus = sound_control_bus_create(&f->control, "app", &command, f->control.app_generation);
    sound_control_exit(&f->control);
    TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, engine));
    TEST_ASSERT_TRUE(f->control.buses[engine_bus.id].used);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, app));
    TEST_ASSERT_FALSE(f->control.buses[app_bus.id].used);
}

static void
test_sound_stopped_warming_amp_turns_off_and_rewarms(void) {
    fixture_t* f = fixture();
    const sound_voice_t voice = play(f, NULL);
    render(f, 0, 1);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_WARMING, f->mixer.amp);
    sound_command_t command;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_STOP, 0, &command));
    render(f, SOUND_AMP_IDLE_MS, 1);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_OFF, f->mixer.amp);
    (void)play(f, NULL);
    render(f, SOUND_AMP_IDLE_MS + 1, 1);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_WARMING, f->mixer.amp);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.cursor.position);
}

static void
test_sound_exact_public_boundaries_and_app_generation_wrap(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_play_opts_t invalid = {.bus = SOUND_BUS_MASTER, .gain_pct = SOUND_GAIN_MAX_PCT};
    TEST_ASSERT_EQUAL_UINT32(
        0, sound_control_play(&f->control, &f->sound, &invalid, 0, 0, &command, f->control.app_generation).id);
    const sound_voice_t voice = play(f, NULL);
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_GAIN, SOUND_GAIN_MAX_PCT, &command));
    for (int i = 0; i < SOUND_BUS_BUILTIN_COUNT; i++) {
        const sound_bus_t bus = {.id = (uint8_t)i};
        TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
        TEST_ASSERT_TRUE(
            sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_GAIN, SOUND_GAIN_MAX_PCT, &command));
    }
    TEST_ASSERT_TRUE(
        sound_control_bus_command(&f->control, SOUND_BUS_MASTER, SOUND_COMMAND_BUS_GAIN, SOUND_GAIN_MAX_PCT, &command));
    f->control.next_generation = UINT32_MAX;
    sound_control_begin_app(&f->control);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, f->control.app_generation);
    sound_control_begin_app(&f->control);
    TEST_ASSERT_EQUAL_UINT32(SOUND_APP_GENERATION_FIRST, f->control.app_generation);
    sound_control_begin_app(&f->control);
    TEST_ASSERT_EQUAL_UINT32(SOUND_APP_GENERATION_FIRST + 1, f->control.app_generation);
}

static void
test_sound_capture_acquire_waits_for_current_owner(void) {
    sound_capture_t capture = {0};
    const uint32_t reader = sound_capture_begin(&capture);
    const int64_t deadline = SOUND_CAPTURE_TIMEOUT_MS * (int64_t)MATH_US_PER_MS;
    uint32_t sequence = 0;
    TEST_ASSERT_EQUAL_INT(SOUND_CAPTURE_WAIT, sound_capture_acquire(&capture, 0, deadline, &sequence));
    TEST_ASSERT_EQUAL_UINT32(0, sequence);
    TEST_ASSERT_EQUAL_UINT32(reader, capture.requested);
    sound_capture_resume(&capture, reader);
    TEST_ASSERT_EQUAL_INT(SOUND_CAPTURE_ACQUIRED,
                          sound_capture_acquire(&capture, deadline - MATH_US_PER_MS, deadline, &sequence));
    TEST_ASSERT_NOT_EQUAL(reader, sequence);
    TEST_ASSERT_TRUE(sound_capture_park(&capture, sequence));
    sound_capture_resume(&capture, sequence);
}

static void
test_sound_capture_acquire_timeout_preserves_current_owner(void) {
    sound_capture_t capture = {0};
    const uint32_t reader = sound_capture_begin(&capture);
    TEST_ASSERT_TRUE(sound_capture_park(&capture, reader));
    const int64_t deadline = SOUND_CAPTURE_TIMEOUT_MS * (int64_t)MATH_US_PER_MS;
    uint32_t sequence = 0;
    TEST_ASSERT_EQUAL_INT(SOUND_CAPTURE_TIMED_OUT, sound_capture_acquire(&capture, deadline, deadline, &sequence));
    TEST_ASSERT_EQUAL_UINT32(0, sequence);
    TEST_ASSERT_TRUE(sound_capture_is_parked(&capture, reader));
    sound_capture_resume(&capture, reader);
    TEST_ASSERT_EQUAL_INT(SOUND_CAPTURE_TIMED_OUT, sound_capture_acquire(&capture, deadline, deadline, &sequence));
    TEST_ASSERT_EQUAL_UINT32(0, capture.requested);
}

static void
test_sound_capture_sequence_wrap_and_results(void) {
    sound_capture_t capture = {0};
    TEST_ASSERT_FALSE(sound_capture_matches(sound_capture_result(0, true), 0));
    const uint32_t edges[] = {0, 1, SOUND_CAPTURE_SEQUENCE_MAX - 1, SOUND_CAPTURE_SEQUENCE_MAX};
    for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        capture.sequence = edges[i];
        const uint32_t sequence = sound_capture_begin(&capture);
        TEST_ASSERT_EQUAL_UINT32(edges[i] == SOUND_CAPTURE_SEQUENCE_MAX ? 1 : edges[i] + 1, sequence);
        const uint32_t ok = sound_capture_result(sequence, true);
        const uint32_t failed = sound_capture_result(sequence, false);
        TEST_ASSERT_EQUAL_UINT32(sequence * (SOUND_CAPTURE_OK + 1u) + SOUND_CAPTURE_OK, ok);
        TEST_ASSERT_TRUE(sound_capture_matches(ok, sequence));
        TEST_ASSERT_TRUE(sound_capture_matches(failed, sequence));
        TEST_ASSERT_TRUE(sound_capture_succeeded(ok, sequence));
        TEST_ASSERT_FALSE(sound_capture_succeeded(failed, sequence));
        sound_capture_resume(&capture, sequence);
    }
}

static void
test_sound_capture_park_resume_holds_voice_position(void) {
    fixture_t* f = fixture();
    (void)play(f, NULL);
    render(f, 0, 1);
    render(f, TEST_STARTUP_MS, 1);
    const sound_mix_voice_t before = f->mixer.voices[0];
    sound_capture_t capture = {0};
    const uint32_t sequence = sound_capture_begin(&capture);
    TEST_ASSERT_FALSE(sound_capture_is_parked(&capture, sequence));
    TEST_ASSERT_TRUE(sound_capture_park(&capture, sequence));
    TEST_ASSERT_EQUAL_UINT32(0, sound_capture_begin(&capture));
    TEST_ASSERT_TRUE(sound_capture_is_parked(&capture, sequence));
    TEST_ASSERT_EQUAL_MEMORY(&before, &f->mixer.voices[0], sizeof before);
    sound_capture_resume(&capture, sequence);
    TEST_ASSERT_FALSE(sound_capture_is_parked(&capture, sequence));
    render(f, TEST_STARTUP_MS + 1, 1);
    TEST_ASSERT_EQUAL_UINT32(before.source.cursor.position + 1, f->mixer.voices[0].source.cursor.position);
}

static void
test_sound_capture_timeout_before_park_rejects_late_command(void) {
    sound_capture_t capture = {0};
    const uint32_t timed_out = sound_capture_begin(&capture);
    sound_capture_resume(&capture, timed_out);
    TEST_ASSERT_FALSE(sound_capture_park(&capture, timed_out));
    const uint32_t next = sound_capture_begin(&capture);
    TEST_ASSERT_NOT_EQUAL(timed_out, next);
    TEST_ASSERT_FALSE(sound_capture_park(&capture, timed_out));
    TEST_ASSERT_FALSE(sound_capture_succeeded(sound_capture_result(timed_out, true), next));
    TEST_ASSERT_TRUE(sound_capture_park(&capture, next));
    sound_capture_resume(&capture, timed_out);
    TEST_ASSERT_TRUE(sound_capture_is_parked(&capture, next));
    sound_capture_resume(&capture, next);
    TEST_ASSERT_FALSE(sound_capture_is_parked(&capture, next));
}

static void
test_sound_capture_timeout_after_park_releases_mixer(void) {
    sound_capture_t capture = {0};
    const uint32_t timed_out = sound_capture_begin(&capture);
    TEST_ASSERT_TRUE(sound_capture_park(&capture, timed_out));
    const uint32_t late = sound_capture_result(timed_out, true);
    sound_capture_resume(&capture, timed_out);
    TEST_ASSERT_FALSE(sound_capture_is_parked(&capture, timed_out));
    const uint32_t next = sound_capture_begin(&capture);
    TEST_ASSERT_FALSE(sound_capture_matches(late, next));
    TEST_ASSERT_FALSE(sound_capture_succeeded(late, next));
    TEST_ASSERT_TRUE(sound_capture_park(&capture, next));
    TEST_ASSERT_FALSE(sound_capture_is_parked(&capture, timed_out));
    TEST_ASSERT_TRUE(sound_capture_succeeded(sound_capture_result(next, true), next));
    sound_capture_resume(&capture, next);
    TEST_ASSERT_FALSE(sound_capture_park(&capture, 0));
    TEST_ASSERT_FALSE(sound_capture_is_parked(&capture, 0));
}

#define TEST_DECLICK_FRAMES 8u
#define TEST_DECLICK_BOUND  (TEST_DECLICK_FRAMES * (SOUND_GAIN_SHIFT + 1u))

static void
test_sound_declick_start_stop_and_tail(void) {
    fixture_t* f = fixture();
    f->mixer.declick_frames = TEST_DECLICK_FRAMES;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    const sound_voice_t voice = play(f, &opts);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(0, f->out[0]);
    const int16_t last = f->out[TEST_FRAMES - 1];
    sound_command_t command;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_STOP, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    render(f, TEST_STARTUP_MS + 1, TEST_FRAMES);
    TEST_ASSERT_INT_WITHIN(TEST_SAMPLE / TEST_DECLICK_FRAMES + 1, last, f->out[0]);
    TEST_ASSERT_GREATER_THAN(0, f->out[0]);
    for (unsigned i = 0; i < TEST_DECLICK_BOUND; i++) {
        render(f, TEST_STARTUP_MS + 1, 1);
    }
    TEST_ASSERT_EQUAL_INT32(0, f->mixer.residual);
    TEST_ASSERT_EQUAL_INT16(0, f->out[0]);
}

static void
test_sound_timed_tone_declick_at_cut(void) {
    fixture_t* f = fixture();
    const size_t final_blocks[] = {1, TEST_FRAMES};
    for (size_t i = 0; i < sizeof final_blocks / sizeof final_blocks[0]; i++) {
        f->mixer.declick_frames = TEST_DECLICK_FRAMES;
        const sound_voice_t voice = play_tone(f, TEST_TONE_MS, SOUND_BUS_SYSTEM);
        for (size_t j = 0; j < TEST_TONE_FRAMES - 1; j++) {
            render(f, TEST_STARTUP_MS, 1);
            TEST_ASSERT_TRUE(sound_control_is_playing(&f->control, voice));
        }
        render(f, TEST_STARTUP_MS, final_blocks[i]);
        const int16_t last = f->out[0];
        TEST_ASSERT_NOT_EQUAL(0, last);
        TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
        if (final_blocks[i] == 1) {
            render(f, TEST_STARTUP_MS, TEST_FRAMES);
            TEST_ASSERT_EQUAL_INT16(last, f->out[0]);
        } else {
            TEST_ASSERT_EQUAL_INT16(last, f->out[1]);
        }
        for (unsigned j = 0; j < TEST_DECLICK_BOUND; j++) {
            render(f, TEST_STARTUP_MS, 1);
        }
        TEST_ASSERT_EQUAL_INT32(0, f->mixer.residual);
        TEST_ASSERT_EQUAL_INT16(0, f->out[0]);
        f->mixer.amp = SOUND_AMP_OFF;
    }
}

static void
test_sound_declick_midblock_end(void) {
    fixture_t* f = fixture();
    f->mixer.declick_frames = TEST_DECLICK_FRAMES;
    f->sound.frames = TEST_FRAMES / 2;
    (void)play(f, NULL);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16(0, f->out[0]);
    TEST_ASSERT_GREATER_THAN(0, f->out[f->sound.frames]);
    for (int i = 1; i < TEST_FRAMES; i++) {
        TEST_ASSERT_INT_WITHIN(TEST_SAMPLE / TEST_DECLICK_FRAMES + 1, f->out[i - 1], f->out[i]);
    }
}

static void
test_sound_declick_cuts_preserve_continuity(void) {
    fixture_t* f = fixture();
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    const sound_command_kind_t cuts[] = {SOUND_COMMAND_PLAY, SOUND_COMMAND_BUS_DESTROY, SOUND_COMMAND_APP_EXIT,
                                         SOUND_COMMAND_STOP};
    for (size_t cut = 0; cut < sizeof cuts / sizeof cuts[0]; cut++) {
        sound_control_init(&f->control, true);
        sound_control_begin_app(&f->control);
        sound_mixer_init(&f->mixer);
        f->mixer.declick_frames = TEST_DECLICK_FRAMES;
        const sound_voice_t voice = play(f, &opts);
        render(f, 0, TEST_FRAMES);
        for (unsigned i = 0; i < TEST_DECLICK_BOUND; i++) {
            render(f, TEST_STARTUP_MS, 1);
        }
        const int16_t last = f->out[0];
        sound_command_t command = {.kind = SOUND_COMMAND_PLAY,
                                   .voice = voice,
                                   .app_generation = f->control.app_generation,
                                   .bus = SOUND_BUS_SFX,
                                   .gain_pct = 0,
                                   .sound = &f->sound,
                                   .loop = true};
        if (cuts[cut] == SOUND_COMMAND_PLAY) {
            sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
        } else if (cuts[cut] == SOUND_COMMAND_BUS_DESTROY) {
            command.kind = SOUND_COMMAND_BUS_DESTROY;
            sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
        } else if (cuts[cut] == SOUND_COMMAND_APP_EXIT) {
            sound_control_exit(&f->control);
            sound_mixer_sync(&f->mixer, &f->control);
        } else {
            command.kind = SOUND_COMMAND_STOP;
            sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
        }
        sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, TEST_FRAMES, TEST_STARTUP_MS,
                           TEST_STARTUP_MS);
        TEST_ASSERT_EQUAL_INT16(last, f->out[0]);
        for (int i = 1; i < TEST_FRAMES; i++) {
            TEST_ASSERT_INT_WITHIN(TEST_SAMPLE / TEST_DECLICK_FRAMES + 1, f->out[i - 1], f->out[i]);
        }
    }
}

static void
test_sound_declick_zero_signal_and_authored_loop_seam(void) {
    fixture_t* f = fixture();
    f->mixer.declick_frames = TEST_DECLICK_FRAMES;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = 0, .loop = true};
    const sound_voice_t voice = play(f, &opts);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT32(0, f->mixer.residual);
    for (int i = 0; i < TEST_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT16(0, f->out[i]);
    }
    sound_command_t command;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_GAIN, SOUND_GAIN_MAX_PCT, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    f->sound.loop_start = 0;
    f->sound.loop_end = TEST_FRAMES / 2;
    for (int i = 0; i < TEST_FRAMES; i++) {
        const int16_t sample = i < TEST_FRAMES / 4 ? TEST_SAMPLE : -TEST_SAMPLE;
        f->pcm[i * SOUND_PCM_BYTES] = (uint8_t)sample;
        f->pcm[i * SOUND_PCM_BYTES + 1] = (uint8_t)((uint16_t)sample >> CHAR_BIT);
    }
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    const unsigned seam = f->sound.loop_end;
    TEST_ASSERT_EQUAL_INT16(-TEST_SAMPLE, f->out[seam - 1]);
    TEST_ASSERT_EQUAL_INT16(TEST_SAMPLE, f->out[seam]);
    TEST_ASSERT_EQUAL_INT32(0, f->mixer.residual);
}

static void
test_sound_declick_amp_off_discards_tail(void) {
    fixture_t* f = fixture();
    f->mixer.declick_frames = SOUND_GAIN_ONE;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    const sound_voice_t voice = play(f, &opts);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    sound_command_t command;
    TEST_ASSERT_TRUE(sound_control_voice_command(&f->control, voice, SOUND_COMMAND_STOP, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_NOT_EQUAL(0, f->mixer.residual);
    render(f, TEST_STARTUP_MS + SOUND_AMP_IDLE_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT(SOUND_AMP_OFF, f->mixer.amp);
    TEST_ASSERT_EQUAL_INT32(0, f->mixer.residual);
}

enum {
    BLOCK_FRAMES = 128,
    SOURCE_FRAMES = 259,
    BLOCKS = 9,
    SEEDS = 3,
    STOP_BLOCK = 3,
    STEAL_BLOCK = 1,
    RESTART_BLOCK = 5,
    STEAL_HANDLE = SOUND_VOICE_COUNT + 1,
    RESTART_HANDLE = STEAL_HANDLE + SOUND_VOICE_COUNT,
    DECLICK_MODE = 1U << 0,
    MASTER_MODE = 1U << 1,
    BUS_MODE = 1U << 2,
    MODE_COUNT = BUS_MODE << 1,
    ADPCM_BYTES = AUDIO_ADPCM_BLOCK_HEADER_BYTES + (SOURCE_FRAMES - 1) / AUDIO_ADPCM_CODES_PER_BYTE
};

typedef struct {
    sound_mixer_t actual, expected;
    uint8_t pcm[SOURCE_FRAMES * SOUND_PCM_BYTES];
    uint8_t adpcm[ADPCM_BYTES];
    int16_t out[BLOCK_FRAMES], reference[BLOCK_FRAMES], scratch[BLOCK_FRAMES];
    int32_t accumulator[BLOCK_FRAMES];
} comparison_t;

static void
seed_comparison(comparison_t* f, unsigned seed) {
    rng_t random;
    rng_seed(&random, seed);
    for (unsigned i = 0; i < sizeof f->pcm; i++) {
        f->pcm[i] = (uint8_t)rng_next(&random);
    }
    for (unsigned i = 0; i < sizeof f->adpcm; i++) {
        f->adpcm[i] = (uint8_t)rng_next(&random);
    }
    f->adpcm[AUDIO_ADPCM_AT_STEP_INDEX] = AUDIO_ADPCM_STEP_INDEX_MAX;
    f->adpcm[AUDIO_ADPCM_AT_RESERVED] = 0;
    if (seed == 1) {
        for (unsigned i = 0; i < SOURCE_FRAMES; i++) {
            const int16_t sample = i < BLOCK_FRAMES ? INT16_MAX : INT16_MIN;
            f->pcm[i * SOUND_PCM_BYTES] = (uint8_t)sample;
            f->pcm[i * SOUND_PCM_BYTES + 1] = (uint8_t)((uint16_t)sample >> CHAR_BIT);
        }
    }
}

static void
start_comparison(comparison_t* f, unsigned seed, unsigned voices, uint8_t gain, unsigned mode, const sound_t* pcm,
                 const sound_t* adpcm) {
    sound_mixer_init(&f->actual);
    f->actual.declick_frames = mode & DECLICK_MODE ? BLOCK_FRAMES : 0;
    f->actual.master_gain = mode & MASTER_MODE ? SOUND_GAIN_MAX_PCT / 2 : SOUND_GAIN_MAX_PCT;
    f->actual.bus_gain[SOUND_BUS_SFX.id] = mode & BUS_MODE ? SOUND_GAIN_MAX_PCT / 2 : SOUND_GAIN_MAX_PCT;
    for (unsigned v = 0; v < voices; v++) {
        sound_command_t command = {.kind = SOUND_COMMAND_PLAY,
                                   .app_generation = SOUND_APP_GENERATION_ENGINE,
                                   .voice = {.id = v + 1},
                                   .bus = SOUND_BUS_SFX,
                                   .gain_pct = gain,
                                   .loop = seed != SEEDS,
                                   .sound = seed != 1 && ((v + seed) & 1U) == 0 ? adpcm : pcm};
        sound_mixer_apply(&f->actual, &command, SOUND_APP_GENERATION_ENGINE);
    }
}

static void
command_comparison(comparison_t* f, unsigned voices, uint8_t gain, unsigned block, const sound_t* adpcm) {
    if (voices && (block == STOP_BLOCK || block == STEAL_BLOCK || block == RESTART_BLOCK)) {
        sound_command_t command = {.kind = block == STOP_BLOCK ? SOUND_COMMAND_STOP : SOUND_COMMAND_PLAY,
                                   .app_generation = SOUND_APP_GENERATION_ENGINE,
                                   .voice = {.id = block == RESTART_BLOCK ? RESTART_HANDLE : STEAL_HANDLE},
                                   .bus = SOUND_BUS_SFX,
                                   .gain_pct = gain,
                                   .loop = false,
                                   .sound = adpcm};
        sound_mixer_apply(&f->actual, &command, SOUND_APP_GENERATION_ENGINE);
        sound_mixer_apply(&f->expected, &command, SOUND_APP_GENERATION_ENGINE);
    }
}

static void
compare_render_block(comparison_t* f, unsigned block) {
    sound_mixer_render(&f->actual, f->out, f->accumulator, f->scratch, BLOCK_FRAMES, block, 0);
    oracle_sound_mixer_render(&f->expected, f->reference, f->accumulator, f->scratch, BLOCK_FRAMES, block, 0);
    TEST_ASSERT_EQUAL_INT16_ARRAY(f->reference, f->out, BLOCK_FRAMES);
    TEST_ASSERT_EQUAL_INT(f->expected.amp, f->actual.amp);
    TEST_ASSERT_EQUAL_INT32(f->expected.residual, f->actual.residual);
    for (unsigned v = 0; v < SOUND_VOICE_COUNT; v++) {
        const sound_mix_voice_t* expected = &f->expected.voices[v];
        const sound_mix_voice_t* actual = &f->actual.voices[v];
        TEST_ASSERT_EQUAL(expected->playing, actual->playing);
        TEST_ASSERT_EQUAL(expected->first_render, actual->first_render);
        TEST_ASSERT_EQUAL_INT32(expected->last_sample, actual->last_sample);
        TEST_ASSERT_EQUAL_UINT32(expected->source.cursor.position, actual->source.cursor.position);
    }
}

static void
check_render_oracle(unsigned voices) {
    comparison_t* f = malloc(sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    static const uint8_t gains[] = {0, 1, SOUND_GAIN_MAX_PCT / 2, SOUND_GAIN_MAX_PCT};
    const sound_t pcm = {
        .data = f->pcm, .frames = SOURCE_FRAMES, .loop_start = 1, .loop_end = SOURCE_FRAMES - 1, .format = SOUND_PCM16};
    const sound_t adpcm = {
        .data = f->adpcm, .frames = SOURCE_FRAMES, .block_frames = SOURCE_FRAMES, .format = SOUND_IMA_ADPCM};
    for (unsigned seed = 1; seed <= SEEDS; seed++) {
        seed_comparison(f, seed);
        for (unsigned g = 0; g < sizeof gains / sizeof gains[0]; g++) {
            for (unsigned mode = 0; mode < MODE_COUNT; mode++) {
                start_comparison(f, seed, voices, gains[g], mode, &pcm, &adpcm);
                f->expected = f->actual;
                for (unsigned block = 0; block < BLOCKS; block++) {
                    command_comparison(f, voices, gains[g], block, &adpcm);
                    compare_render_block(f, block);
                }
            }
        }
    }
    free(f);
}

static void
test_render_no_voices_matches_oracle(void) {
    check_render_oracle(0);
}

static void
test_render_one_voice_matches_oracle(void) {
    check_render_oracle(1);
}

static void
test_render_two_voices_matches_oracle(void) {
    check_render_oracle(2);
}

static void
test_render_all_voices_matches_oracle(void) {
    check_render_oracle(SOUND_VOICE_COUNT);
}

static void
test_empty_loop_matches_oracle(void) {
    fixture_t* f = fixture();
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT, .loop = true};
    (void)play(f, &opts);
    f->sound.frames = 0;
    sound_mixer_t* expected = malloc(sizeof *expected);
    TEST_ASSERT_NOT_NULL(expected);
    *expected = f->mixer;
    memset(f->out, 1, sizeof f->out);
    oracle_sound_mixer_render(expected, f->expected, f->accumulator, f->scratch, TEST_FRAMES, 0, 0);
    sound_mixer_render(&f->mixer, f->out, f->accumulator, f->scratch, TEST_FRAMES, 0, 0);
    TEST_ASSERT_EQUAL_INT16_ARRAY(f->expected, f->out, TEST_FRAMES);
    TEST_ASSERT_FALSE(f->mixer.voices[0].playing);
    free(expected);
}

void
run_sound_mixer_suite(void) {
    RUN_TEST(test_empty_loop_matches_oracle);
    RUN_TEST(test_render_no_voices_matches_oracle);
    RUN_TEST(test_render_one_voice_matches_oracle);
    RUN_TEST(test_render_two_voices_matches_oracle);
    RUN_TEST(test_render_all_voices_matches_oracle);
    RUN_TEST(test_sound_declick_amp_off_discards_tail);
    RUN_TEST(test_sound_declick_cuts_preserve_continuity);
    RUN_TEST(test_sound_declick_zero_signal_and_authored_loop_seam);
    RUN_TEST(test_sound_declick_start_stop_and_tail);
    RUN_TEST(test_sound_declick_midblock_end);
    RUN_TEST(test_sound_timed_tone_declick_at_cut);
    RUN_TEST(test_sound_timed_tone_ends_on_time);
    RUN_TEST(test_sound_tone_uses_requested_bus_gain_and_mute);
    RUN_TEST(test_sound_tone_until_stopped);
    RUN_TEST(test_sound_capture_acquire_waits_for_current_owner);
    RUN_TEST(test_sound_capture_acquire_timeout_preserves_current_owner);
    RUN_TEST(test_sound_capture_sequence_wrap_and_results);
    RUN_TEST(test_sound_capture_park_resume_holds_voice_position);
    RUN_TEST(test_sound_capture_timeout_before_park_rejects_late_command);
    RUN_TEST(test_sound_capture_timeout_after_park_releases_mixer);
    RUN_TEST(test_sound_exit_window_rejects_controls_and_none_commands);
    RUN_TEST(test_sound_producer_admission_cannot_cross_teardown);
    RUN_TEST(test_sound_stale_bus_commands_cannot_change_next_session);
    RUN_TEST(test_sound_completion_preserves_a_queued_replacement);
    RUN_TEST(test_sound_sync_stops_an_unreplaced_renderer_handle);
    RUN_TEST(test_sound_none_generation_never_has_a_pending_read);
    RUN_TEST(test_sound_exit_preserves_engine_owners_and_retires_only_app_owners);
    RUN_TEST(test_sound_stopped_warming_amp_turns_off_and_rewarms);
    RUN_TEST(test_sound_exact_public_boundaries_and_app_generation_wrap);

    RUN_TEST(test_sound_exit_resets_app_gains_and_preserves_system_gain);
    RUN_TEST(test_sound_gain_sweep_uses_each_stage_and_bus);
    RUN_TEST(test_sound_bus_reuse_resets_render_gain);
    RUN_TEST(test_sound_delayed_controls_ignore_reused_slot);
    RUN_TEST(test_sound_exit_never_reads_retired_source_and_preserves_engine);
    RUN_TEST(test_sound_amp_rewarms_and_idle_window_refreshes);
    RUN_TEST(test_sound_free_slot_precedes_steal_and_handles_wrap);
    RUN_TEST(test_sound_invalid_arguments_leave_control_and_command_unchanged);
    RUN_TEST(test_sound_authored_loop_crosses_blocks_and_empty_read_stops);
    RUN_TEST(test_sound_saturation_exact_rails_and_one_over);
    RUN_TEST(test_sound_command_queue_round_trip);
    RUN_TEST(test_sound_exit_closes_play_and_stops_app_system);
    RUN_TEST(test_sound_exit_waits_only_for_the_retired_read_epoch);
    RUN_TEST(test_sound_borrows_view_and_retires_a_stopped_queued_steal);
    RUN_TEST(test_sound_tone_source_holds_phase_until_warm);
    RUN_TEST(test_sound_warmup_holds_positions_and_writes_silence);
    RUN_TEST(test_sound_saturates_both_rails);
    RUN_TEST(test_sound_voice_bus_and_master_gain);
    RUN_TEST(test_sound_steals_lowest_priority_then_oldest);
    RUN_TEST(test_sound_stop_and_completion_do_not_resurrect_handles);
    RUN_TEST(test_sound_bus_capacity_destroy_and_reuse);
    RUN_TEST(test_sound_exit_drops_queued_app_commands_preserves_system);
    RUN_TEST(test_sound_loops_seek_and_honors_authored_loop_end);
    RUN_TEST(test_sound_failed_init_is_noop);
}
SUITE_REGISTER(run_sound_mixer_suite)
