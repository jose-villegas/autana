/* The control plane and PCM renderer run independently of an RTOS or codec. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sound/sound_mixer.h"
#include "suites.h"
#include "test_cleanup.h"
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
    const sound_voice_t voice = sound_control_play(&f->control, &f->sound, opts, 0, &command);
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
        buses[i] = sound_control_bus_create(&f->control, "test", &command);
        TEST_ASSERT_EQUAL_INT(i, buses[i].id);
        sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    }
    TEST_ASSERT_EQUAL_INT(SOUND_BUS_INVALID.id, sound_control_bus_create(&f->control, "full", &command).id);
    const sound_bus_t bus = buses[SOUND_BUS_BUILTIN_COUNT];
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = SOUND_GAIN_MAX_PCT};
    const sound_voice_t voice = play(f, &opts);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, voice));
    TEST_ASSERT_EQUAL_INT(bus.id, sound_control_bus_create(&f->control, "reuse", &command).id);
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, SOUND_BUS_SYSTEM, SOUND_COMMAND_BUS_DESTROY, 0, &command));
}

static void
test_sound_exit_drops_queued_app_commands_preserves_system(void) {
    fixture_t* f = fixture();
    sound_command_t app_command, system_command;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT};
    sound_control_end_app(&f->control);
    const sound_voice_t system = sound_control_play(&f->control, &f->sound, &opts, 0, &system_command);
    sound_control_begin_app(&f->control);
    const sound_voice_t app = sound_control_play(&f->control, &f->sound, NULL, 0, &app_command);
    const sound_bus_t bus = sound_control_bus_create(&f->control, "app", &(sound_command_t){0});
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
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, NULL, 0, &command).id);
    TEST_ASSERT_FALSE(sound_control_is_playing(&f->control, SOUND_VOICE_NULL));
    TEST_ASSERT_FALSE(
        sound_control_voice_command(&f->control, (sound_voice_t){.id = 1}, SOUND_COMMAND_STOP, 0, &command));
    TEST_ASSERT_EQUAL_INT(SOUND_BUS_INVALID.id, sound_control_bus_create(&f->control, "no", &command).id);
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, SOUND_BUS_SFX, SOUND_COMMAND_BUS_GAIN, 0, &command));
    const uint32_t generation = f->control.app_generation;
    sound_control_exit(&f->control);
    TEST_ASSERT_EQUAL_UINT32(generation, f->control.app_generation);
}

static void
test_sound_tone_source_holds_phase_until_warm(void) {
    fixture_t* f = fixture();
    sound_command_t command;
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT};
    const sound_voice_t voice = sound_control_play(&f->control, NULL, &opts, TEST_TONE_HZ, &command);
    TEST_ASSERT_NOT_EQUAL(0, voice.id);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    render(f, 0, TEST_FRAMES);
    render(f, TEST_STARTUP_MS - 1, TEST_FRAMES);
    TEST_ASSERT_EQUAL_UINT32(0, f->mixer.voices[0].source.tone.phase);
    TEST_ASSERT_FALSE(sound_control_is_audible(&f->control, voice));
    audio_tone_t expected;
    audio_tone_init(&expected, TEST_TONE_HZ);
    audio_tone_fill(&expected, f->expected, TEST_FRAMES, AUDIO_OUT_VOLUME_MAX_PCT);
    render(f, TEST_STARTUP_MS, TEST_FRAMES);
    TEST_ASSERT_EQUAL_INT16_ARRAY(f->expected, f->out, TEST_FRAMES);
    TEST_ASSERT_TRUE(sound_control_is_audible(&f->control, voice));
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
    const sound_voice_t next = sound_control_play(&f->control, &f->sound, NULL, 0, &pending);
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
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, &opts, 0, &command).id);
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, NULL, 0, &command).id);
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
    const sound_bus_t created = sound_control_bus_create(&f->control, "gain", &command);
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
    const sound_bus_t bus = sound_control_bus_create(&f->control, "first", &command);
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_GAIN, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_EQUAL_UINT8(bus.id, sound_control_bus_create(&f->control, "next", &command).id);
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
    const sound_bus_t bus = sound_control_bus_create(&f->control, "owned", &(sound_command_t){0});
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
        handles[slot] = sound_control_play(&f->control, &f->sound, NULL, 0, &command);
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
    sound_command_t command = {.kind = SOUND_COMMAND_CAPTURE_CLOSE};
    const sound_command_t before = command;
    const sound_control_t saved = f->control;
    sound_play_opts_t opts = {.bus = SOUND_BUS_SFX, .gain_pct = SOUND_GAIN_MAX_PCT + 1};
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, &opts, 0, &command).id);
    opts.gain_pct = SOUND_GAIN_MAX_PCT;
    opts.bus = SOUND_BUS_INVALID;
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, &opts, 0, &command).id);
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, NULL, NULL, 0, &command).id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_INVALID.id, sound_control_bus_create(&f->control, "", &command).id);
    char name[SOUND_BUS_NAME_MAX + 1];
    memset(name, 'x', sizeof name);
    name[sizeof name - 1] = '\0';
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_INVALID.id, sound_control_bus_create(&f->control, name, &command).id);
    TEST_ASSERT_EQUAL_MEMORY(&saved, &f->control, sizeof saved);
    TEST_ASSERT_EQUAL_MEMORY(&before, &command, sizeof before);
    name[SOUND_BUS_NAME_MAX - 1] = '\0';
    const sound_bus_t bus = sound_control_bus_create(&f->control, name, &command);
    TEST_ASSERT_NOT_EQUAL(SOUND_BUS_INVALID.id, bus.id);
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_DESTROY, 0, &command));
    TEST_ASSERT_FALSE(sound_control_bus_command(&f->control, bus, SOUND_COMMAND_BUS_GAIN, 0, &command));
    opts.bus = bus;
    TEST_ASSERT_EQUAL_UINT32(0, sound_control_play(&f->control, &f->sound, &opts, 0, &command).id);
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
        TEST_ASSERT_NOT_EQUAL(0, sound_control_play(&f->control, &f->extra_sound, &opts, 0, &command).id);
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
    (void)sound_control_play(&f->control, NULL, &opts, TEST_TONE_HZ, &command);
    uint8_t queued[sizeof command];
    memcpy(queued, &command, sizeof command);
    sound_command_t restored;
    memcpy(&restored, queued, sizeof restored);
    TEST_ASSERT_EQUAL_INT(SOUND_COMMAND_PLAY, restored.kind);
    TEST_ASSERT_TRUE(restored.loop);
    TEST_ASSERT_TRUE(restored.tone_source);
    TEST_ASSERT_EQUAL_UINT32(TEST_TONE_HZ, restored.tone_hz);
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
    TEST_ASSERT_TRUE(sound_control_bus_command(&f->control, SOUND_BUS_SFX, SOUND_COMMAND_BUS_GAIN, 0, &command));
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    sound_control_end_app(&f->control);
    TEST_ASSERT_TRUE(f->control.reset_gains);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->control.buses[SOUND_BUS_SFX.id].gain_pct);
    command = (sound_command_t){.kind = SOUND_COMMAND_APP_EXIT};
    sound_mixer_apply(&f->mixer, &command, f->control.app_generation);
    TEST_ASSERT_EQUAL_UINT8(SOUND_GAIN_MAX_PCT, f->mixer.bus_gain[SOUND_BUS_SFX.id]);
}

void
run_sound_mixer_suite(void) {
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
