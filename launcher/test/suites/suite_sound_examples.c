/* The sound usage examples (sound_examples/) compiled and run against the real control plane and mixer.
 * The app API is replaced by static functions that queue into a sound_control_t and apply to a sound_mixer_t. */
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "sound/sound_mixer.h"
#include "sound/sound_play.h"
#include "suites.h"
#include "test_pack.h"
#include "unity.h"

enum { PACK_CAPACITY = 1024, SOUND_FRAMES = 8, BLOCK = 8, STARTUP_MS = 0, LOOP_END = 4, HALF = SOUND_GAIN_MAX_PCT / 2 };

typedef struct {
    sound_control_t control;
    sound_mixer_t mixer;
    _Alignas(ASSET_PACK_BASE_ALIGN) uint8_t pack_bytes[PACK_CAPACITY];
    asset_pack_t pack;
    int16_t out[BLOCK];
    int32_t accumulator[BLOCK];
    int16_t scratch[BLOCK];
    uint64_t now_ms;
    bool pack_missing;
} rig_t;

static rig_t* rig;

static void
cleanup(void) {
    free(rig);
    rig = NULL;
}

static void
add_sound(test_pack_t* pack, const char* name, uint32_t loop_end) {
    uint8_t* entry = test_pack_add(pack, name, SOUND_ASSET, SOUND_HEADER_SIZE + SOUND_FRAMES * SOUND_PCM_BYTES);
    test_pack_put16(entry + SOUND_AT_VERSION, SOUND_VERSION);
    entry[SOUND_AT_FORMAT] = SOUND_FORMAT_PCM16;
    entry[SOUND_AT_CHANNELS] = SOUND_CHANNELS;
    test_pack_put32(entry + SOUND_AT_SAMPLE_RATE, SOUND_SAMPLE_RATE_HZ);
    test_pack_put32(entry + SOUND_AT_FRAMES, SOUND_FRAMES);
    test_pack_put32(entry + SOUND_AT_LOOP_END, loop_end);
    test_pack_put32(entry + SOUND_AT_DATA, SOUND_HEADER_SIZE);
    for (int i = 0; i < SOUND_FRAMES; i++) {
        test_pack_put16(entry + SOUND_HEADER_SIZE + i * SOUND_PCM_BYTES, 1000);
    }
}

static rig_t*
begin(void) {
    rig = calloc(1, sizeof *rig);
    TEST_ASSERT_NOT_NULL(rig);
    suite_set_test_cleanup(cleanup);
    sound_control_init(&rig->control, true);
    sound_control_begin_app(&rig->control);
    sound_mixer_init(&rig->mixer);
    test_pack_t builder = test_pack_begin(rig->pack_bytes, PACK_CAPACITY, 3);
    add_sound(&builder, "tap", 0);
    add_sound(&builder, "click", 0);
    add_sound(&builder, "theme", LOOP_END);
    const uint32_t size = test_pack_finish(&builder);
    TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(&rig->pack, rig->pack_bytes, size));
    return rig;
}

static void
send(const sound_command_t* command) {
    sound_mixer_apply(&rig->mixer, command, rig->control.app_generation);
}

static const asset_pack_t*
fake_pack(const char* name) {
    (void)name;
    return rig->pack_missing ? NULL : &rig->pack;
}

static sound_voice_t
fake_play(const sound_t* sound, const sound_play_opts_t* opts) {
    sound_command_t command;
    const sound_voice_t voice =
        sound_control_play(&rig->control, sound, opts, 0, &command, rig->control.app_generation);
    if (voice.id) {
        send(&command);
    }
    return voice;
}

static void
fake_voice_command(sound_voice_t voice, sound_command_kind_t kind, uint8_t gain) {
    sound_command_t command;
    if (sound_control_voice_command(&rig->control, voice, kind, gain, &command)) {
        send(&command);
    }
}

static void
fake_stop(sound_voice_t voice) {
    fake_voice_command(voice, SOUND_COMMAND_STOP, 0);
}

static void
fake_bus_gain(sound_bus_t bus, uint8_t pct) {
    sound_command_t command;
    if (sound_control_bus_command(&rig->control, bus, SOUND_COMMAND_BUS_GAIN, pct, &command)) {
        send(&command);
    }
}

#define asset_store_pack(name)       fake_pack(name)
#define sound_play(sound, opts)      fake_play(sound, opts)
#define sound_stop(voice)            fake_stop(voice)
#define sound_set_bus_gain(bus, pct) fake_bus_gain(bus, pct)

#include "../sound_examples/music.c"
#include "../sound_examples/own.c"
#include "../sound_examples/play.c"

#undef asset_store_pack
#undef sound_play
#undef sound_stop
#undef sound_set_bus_gain

static void
render(void) {
    sound_mixer_sync(&rig->mixer, &rig->control);
    sound_mixer_render(&rig->mixer, rig->out, rig->accumulator, rig->scratch, BLOCK, rig->now_ms, STARTUP_MS);
    sound_control_complete(&rig->control, &rig->mixer);
    rig->now_ms += BLOCK;
}

static void
test_tap_plays_once_on_sfx_and_stops(void) {
    begin();
    play_example_t s = {0};
    play_example_open(&s);
    TEST_ASSERT_NOT_NULL(s.tap.data);
    play_example_tap(&s);
    TEST_ASSERT_NOT_EQUAL(0, s.last.id);
    TEST_ASSERT_TRUE(sound_control_is_playing(&rig->control, s.last));
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_SFX.id, rig->control.voices[0].opts.bus.id);
    TEST_ASSERT_FALSE(rig->control.voices[0].opts.loop);
    render();
    play_example_cancel(&s);
    TEST_ASSERT_FALSE(sound_control_is_playing(&rig->control, s.last));
    play_example_cancel(&s); /* a stale handle is ignored */
}

static void
test_unopened_sound_plays_nothing(void) {
    begin();
    play_example_t s = {0};
    play_example_tap(&s);
    TEST_ASSERT_EQUAL_UINT32(0, s.last.id);
}

static void
test_music_loops_and_the_bus_gain_scales_it(void) {
    begin();
    music_example_t s = {0};
    music_example_start(&s, &rig->pack);
    TEST_ASSERT_NOT_EQUAL(0, s.voice.id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_MUSIC.id, rig->control.voices[0].opts.bus.id);
    for (int i = 0; i < 3; i++) {
        render(); /* a one-shot of 8 frames would have ended in the first block */
    }
    TEST_ASSERT_TRUE(rig->mixer.voices[0].playing);
    render();
    const int16_t loud = rig->out[0];
    music_example_quieter();
    TEST_ASSERT_EQUAL_UINT8(HALF, rig->mixer.bus_gain[SOUND_BUS_MUSIC.id]);
    render();
    TEST_ASSERT_TRUE(rig->out[0] < loud);
    music_example_stop(&s);
    TEST_ASSERT_FALSE(sound_control_is_playing(&rig->control, s.voice));
}

static void
test_own_sound_uses_the_system_bus_and_a_missing_one_is_silent(void) {
    begin();
    own_example_t s = {0};
    own_example_open(&s, "myapp");
    TEST_ASSERT_TRUE(s.have_click);
    own_example_confirm(&s);
    TEST_ASSERT_EQUAL_UINT8(SOUND_BUS_SYSTEM.id, rig->control.voices[0].opts.bus.id);
    own_example_t missing = {0};
    rig->pack_missing = true;
    own_example_open(&missing, "myapp");
    TEST_ASSERT_FALSE(missing.have_click);
    own_example_confirm(&missing); /* nothing queued, nothing crashes */
}

void
suite_sound_examples(void) {
    RUN_TEST(test_tap_plays_once_on_sfx_and_stops);
    RUN_TEST(test_unopened_sound_plays_nothing);
    RUN_TEST(test_music_loops_and_the_bus_gain_scales_it);
    RUN_TEST(test_own_sound_uses_the_system_bus_and_a_missing_one_is_silent);
}

SUITE_REGISTER(suite_sound_examples);
