#include <math.h>
#include <stdlib.h>

#include "asset/asset_bytes.h"
#include "asset/asset_store.h"
#include "audio/audio_adpcm.h"
#include "sound/sound.h"
#include "suites.h"
#include "unity.h"
#ifndef DEVICE_BUILD
#include "asset/asset_file.h"
#endif

#include "fixtures/audio_adpcm_reference.h"

static void
check_reference(const uint8_t* block, const int16_t* expected, uint32_t frames) {
    const sound_t sound = {
        .data = block, .frames = frames, .block_frames = (uint16_t)frames, .format = SOUND_IMA_ADPCM};
    sound_cursor_t cursor;
    int16_t* decoded = malloc(frames * sizeof *decoded);
    TEST_ASSERT_NOT_NULL(decoded);
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, 0));
    TEST_ASSERT_EQUAL(frames, sound_read(&cursor, decoded, frames));
    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, decoded, frames);
    free(decoded);
}

static void
test_ffmpeg_reference(void) {
    check_reference(BLOCK, EXPECTED, sizeof EXPECTED / sizeof EXPECTED[0]);
}

static void
test_ffmpeg_saturation_reference(void) {
    check_reference(SATURATION_BLOCK, SATURATION_EXPECTED, sizeof SATURATION_EXPECTED / sizeof SATURATION_EXPECTED[0]);
}

#ifndef DEVICE_BUILD
/* sound_probe.signal() includes index-zero adaptation. Bounds of 300
 * peak and 50 RMS allow that startup transient, yet reject nibble or state errors. */
enum { MAX_ERROR = 300, RMS_ERROR = 50 };

static void
test_baker_multiblock_round_trip(void) {
    const char* path = getenv("AUTANA_SOUND_PROBE");
    TEST_ASSERT_NOT_NULL(path);
    asset_pack_t pack;
    void* bytes = NULL;
    TEST_ASSERT_EQUAL(ASSET_OK, asset_file_open(path, &pack, &bytes));
    sound_t pcm, adpcm;
    TEST_ASSERT_EQUAL(ASSET_OK, sound_open(&pack, "pcm16", &pcm));
    TEST_ASSERT_EQUAL(ASSET_OK, sound_open(&pack, "adpcm", &adpcm));
    int16_t* decoded = malloc(adpcm.frames * sizeof *decoded);
    TEST_ASSERT_NOT_NULL(decoded);
    sound_cursor_t cursor;
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &adpcm, 0));
    TEST_ASSERT_EQUAL(adpcm.frames, sound_read(&cursor, decoded, adpcm.frames));
    double squared = 0;
    int maximum = 0;
    for (uint32_t i = 0; i < pcm.frames; i++) {
        const int sample = sound_read_sample(pcm.data + i * SOUND_PCM_BYTES);
        const int error = abs(sample - decoded[i]);
        maximum = error > maximum ? error : maximum;
        squared += (double)error * error;
    }
    const double rms = sqrt(squared / pcm.frames);
    const bool loop = adpcm.loop_start == adpcm.block_frames && adpcm.loop_end == adpcm.frames;
    const bool same_frames = pcm.frames == adpcm.frames;
    free(decoded);
    asset_file_release(bytes);
    TEST_ASSERT_TRUE(loop);
    TEST_ASSERT_TRUE(same_frames);
    TEST_ASSERT_LESS_OR_EQUAL(MAX_ERROR, maximum);
    TEST_ASSERT_TRUE(rms <= RMS_ERROR);
}
#endif

static void
test_engine_sound(void) {
    const asset_pack_t* pack = asset_store_pack("engine");
    TEST_ASSERT_NOT_NULL(pack);
    sound_t sound;
    const asset_status_t status = sound_open(pack, "tap", &sound);
    asset_store_release("engine");
    TEST_ASSERT_EQUAL(ASSET_OK, status);
    TEST_ASSERT_EQUAL(SOUND_IMA_ADPCM, sound.format);
    TEST_ASSERT_EQUAL(SOUND_DEFAULT_BLOCK_FRAMES, sound.block_frames);
    TEST_ASSERT_GREATER_THAN(0, sound.frames);
    TEST_ASSERT_EQUAL(0, sound.loop_end);
}

void
suite_audio_adpcm(void) {
    RUN_TEST(test_ffmpeg_reference);
    RUN_TEST(test_ffmpeg_saturation_reference);
#ifndef DEVICE_BUILD
    RUN_TEST(test_baker_multiblock_round_trip);
#endif
    RUN_TEST(test_engine_sound);
}

SUITE_REGISTER(suite_audio_adpcm);
SUITE_READS(suite_audio_adpcm, "engine");
