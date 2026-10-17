#include <math.h>
#include <stdlib.h>

#include "asset/asset_bytes.h"
#include "asset/asset_store.h"
#include "audio/audio_adpcm.h"
#include "audio/audio_sound.h"
#include "suites.h"
#include "unity.h"
#ifndef DEVICE_BUILD
#include "asset/asset_file.h"
#endif

/* Input in.raw is LE int16: 0,1000,-1000,2000,-2000,3000,-3000,32767,-32768.
 * ffmpeg -f s16le -ar 22050 -ac 1 -i in.raw -c:a adpcm_ima_wav -block_size 32 out.wav
 * ffmpeg -i out.wav -f s16le decoded.raw
 * BLOCK is out.wav's data chunk; EXPECTED is decoded.raw, including padding. */
static const uint8_t BLOCK[] = {
    0x00, 0x00, 0x00, 0x00, 0xf7, 0xf7, 0xf7, 0xf7, 0x82, 0x80, 0x80, 0x80, 0x00, 0x88, 0x00, 0x88,
    0x00, 0x88, 0x00, 0x88, 0x00, 0x88, 0x80, 0x00, 0x08, 0x88, 0x00, 0x88, 0x00, 0x88, 0x80, 0x80,
};
static const int16_t EXPECTED[] = {
    0,   11,   -19, 44,  -92, 201, -430, 927, -1983, 95,  -283, 60, -252, 32,  -226, 8,  -205, -11, 165,
    5,   -140, -8,  112, 3,   -96, -6,   76,  2,     -66, -5,   51, 0,    -46, -4,   34, 0,    -31, -3,
    -29, -6,   15,  -4,  13,  -3,  -17,  -4,  8,     -3,  -13,  -4, 4,    -3,  -9,   -3, -8,   -3,  -7,
};

static void
test_ffmpeg_reference(void) {
    const size_t frames = sizeof EXPECTED / sizeof EXPECTED[0];
    int16_t* decoded = malloc(sizeof EXPECTED);
    TEST_ASSERT_NOT_NULL(decoded);
    audio_adpcm_decode_block(BLOCK, (uint32_t)frames, decoded);
    bool equal = true;
    for (size_t i = 0; i < frames; i++) {
        equal = equal && decoded[i] == EXPECTED[i];
    }
    free(decoded);
    TEST_ASSERT_TRUE(equal);
}

#ifndef DEVICE_BUILD
/* The smooth 1200-peak fixture includes index-zero adaptation. Bounds of 300
 * peak and 50 RMS allow that startup transient, yet reject nibble or state errors. */
enum { MAX_ERROR = 300, RMS_ERROR = 50 };

static void
test_baker_multiblock_round_trip(void) {
    const char* path = getenv("AUTANA_SOUND_PROBE");
    TEST_ASSERT_NOT_NULL(path);
    asset_pack_t pack;
    void* bytes = NULL;
    TEST_ASSERT_EQUAL(ASSET_OK, asset_file_open(path, &pack, &bytes));
    audio_sound_t pcm, adpcm;
    TEST_ASSERT_EQUAL(ASSET_OK, audio_sound_open(&pack, "pcm16", &pcm));
    TEST_ASSERT_EQUAL(ASSET_OK, audio_sound_open(&pack, "adpcm", &adpcm));
    int16_t* decoded = malloc(adpcm.frames * sizeof *decoded);
    TEST_ASSERT_NOT_NULL(decoded);
    uint32_t at = 0;
    const uint8_t* block = adpcm.data;
    while (at < adpcm.frames) {
        const uint32_t count = adpcm.frames - at < adpcm.block_frames ? adpcm.frames - at : adpcm.block_frames;
        audio_adpcm_decode_block(block, count, decoded + at);
        block += audio_adpcm_block_bytes(adpcm.block_frames);
        at += count;
    }
    double squared = 0;
    int maximum = 0;
    for (uint32_t i = 0; i < pcm.frames; i++) {
        const uint16_t raw = asset_read_u16(pcm.data + i * AUDIO_SOUND_PCM_BYTES);
        const int sample = raw <= INT16_MAX ? (int)raw : (int)raw - (UINT16_MAX + 1);
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
    audio_sound_t sound;
    const asset_status_t status = audio_sound_open(pack, "tap", &sound);
    asset_store_release("engine");
    TEST_ASSERT_EQUAL(ASSET_OK, status);
    TEST_ASSERT_EQUAL(AUDIO_SOUND_IMA_ADPCM, sound.format);
    TEST_ASSERT_EQUAL(AUDIO_SOUND_DEFAULT_BLOCK_FRAMES, sound.block_frames);
    TEST_ASSERT_GREATER_THAN(0, sound.frames);
    TEST_ASSERT_EQUAL(0, sound.loop_end);
}

void
suite_audio_adpcm(void) {
    RUN_TEST(test_ffmpeg_reference);
#ifndef DEVICE_BUILD
    RUN_TEST(test_baker_multiblock_round_trip);
#endif
    RUN_TEST(test_engine_sound);
}

SUITE_REGISTER(suite_audio_adpcm);
SUITE_READS(suite_audio_adpcm, "engine");
