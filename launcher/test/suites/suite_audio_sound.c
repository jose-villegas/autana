#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_adpcm.h"
#include "audio/audio_sound.h"
#include "suites.h"
#include "test_pack.h"
#include "unity.h"

enum { PACK_CAPACITY = 512, FRAMES = 11, BLOCK_FRAMES = 9 };

static const char NAME[] = "sound";

typedef struct {
    uint8_t* bytes;
    uint8_t* entry;
    test_pack_t builder;
} fixture_t;

static fixture_t
fixture(audio_sound_format_t format) {
    fixture_t f = {0};
    f.bytes = malloc(PACK_CAPACITY);
    TEST_ASSERT_NOT_NULL(f.bytes);
    f.builder = test_pack_begin(f.bytes, PACK_CAPACITY, 1);
    const uint32_t data_bytes =
        format == AUDIO_SOUND_PCM16 ? FRAMES * AUDIO_SOUND_PCM_BYTES : 2 * audio_adpcm_block_bytes(BLOCK_FRAMES);
    f.entry = test_pack_add(&f.builder, NAME, AUDIO_SOUND_ASSET,
                            AUDIO_SOUND_HEADER_SIZE + AUDIO_SOUND_ALIGNMENT + data_bytes);
    test_pack_put16(f.entry + AUDIO_SOUND_AT_VERSION, AUDIO_SOUND_VERSION);
    f.entry[AUDIO_SOUND_AT_FORMAT] = (uint8_t)format;
    f.entry[AUDIO_SOUND_AT_CHANNELS] = AUDIO_SOUND_CHANNELS;
    test_pack_put32(f.entry + AUDIO_SOUND_AT_SAMPLE_RATE, AUDIO_SOUND_SAMPLE_RATE_HZ);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_FRAMES, FRAMES);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_DATA, AUDIO_SOUND_HEADER_SIZE + AUDIO_SOUND_ALIGNMENT);
    test_pack_put16(f.entry + AUDIO_SOUND_AT_BLOCK_FRAMES, format == AUDIO_SOUND_PCM16 ? 0 : BLOCK_FRAMES);
    return f;
}

static asset_status_t
open_fixture(fixture_t* f, const char* name, audio_sound_t* sound) {
    asset_pack_t pack;
    const uint32_t size = test_pack_finish(&f->builder);
    TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(&pack, f->bytes, size));
    return audio_sound_open(&pack, name, sound);
}

static bool
sound_empty(const audio_sound_t* sound) {
    return sound->data == NULL && sound->frames == 0 && sound->loop_start == 0 && sound->loop_end == 0
           && sound->block_frames == 0 && sound->format == 0;
}

static void
reject_field(audio_sound_format_t format, uint32_t at, uint32_t value, unsigned width, asset_status_t expected) {
    fixture_t f = fixture(format);
    if (width == sizeof(uint32_t)) {
        test_pack_put32(f.entry + at, value);
    } else if (width == sizeof(uint16_t)) {
        test_pack_put16(f.entry + at, (int)value);
    } else {
        f.entry[at] = (uint8_t)value;
    }
    audio_sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t status = open_fixture(&f, NAME, &sound);
    const bool cleared = sound_empty(&sound);
    free(f.bytes);
    TEST_ASSERT_EQUAL(expected, status);
    TEST_ASSERT_TRUE(cleared);
}

static void
check_valid(audio_sound_format_t format) {
    fixture_t f = fixture(format);
    const uint32_t start = format == AUDIO_SOUND_PCM16 ? 1 : BLOCK_FRAMES;
    test_pack_put32(f.entry + AUDIO_SOUND_AT_LOOP_START, start);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_LOOP_END, FRAMES);
    audio_sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t status = open_fixture(&f, NAME, &sound);
    const bool in_place = sound.data == f.entry + AUDIO_SOUND_HEADER_SIZE + AUDIO_SOUND_ALIGNMENT;
    free(f.bytes);
    TEST_ASSERT_EQUAL(ASSET_OK, status);
    TEST_ASSERT_TRUE(in_place);
    TEST_ASSERT_EQUAL(FRAMES, sound.frames);
    TEST_ASSERT_EQUAL(start, sound.loop_start);
    TEST_ASSERT_EQUAL(FRAMES, sound.loop_end);
    TEST_ASSERT_EQUAL(format, sound.format);
    TEST_ASSERT_EQUAL(format == AUDIO_SOUND_PCM16 ? 0 : BLOCK_FRAMES, sound.block_frames);
}

static void
test_pcm16(void) {
    check_valid(AUDIO_SOUND_PCM16);
}

static void
test_adpcm(void) {
    check_valid(AUDIO_SOUND_IMA_ADPCM);
}

static void
test_channels(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_CHANNELS, 2, 1, ASSET_ERR_FORMAT);
}

static void
test_rate(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_SAMPLE_RATE, AUDIO_SOUND_SAMPLE_RATE_HZ + 1, sizeof(uint32_t),
                 ASSET_ERR_FORMAT);
}

static void
test_format(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_FORMAT, AUDIO_SOUND_FORMAT_IMA_ADPCM + 1, 1, ASSET_ERR_FORMAT);
}

static void
test_reserved(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_RESERVED16, 1, sizeof(uint16_t), ASSET_ERR_FORMAT);
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_RESERVED32, 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
}

static void
test_offset(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_DATA, AUDIO_SOUND_HEADER_SIZE + 1, sizeof(uint32_t),
                 ASSET_ERR_BOUNDS);
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_DATA, AUDIO_SOUND_HEADER_SIZE - AUDIO_SOUND_ALIGNMENT,
                 sizeof(uint32_t), ASSET_ERR_BOUNDS);
}

static void
test_data_bounds(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_FRAMES, UINT32_MAX, sizeof(uint32_t), ASSET_ERR_BOUNDS);
    reject_field(AUDIO_SOUND_IMA_ADPCM, AUDIO_SOUND_AT_FRAMES, UINT32_MAX, sizeof(uint32_t), ASSET_ERR_BOUNDS);
}

static void
test_loop_range(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_LOOP_END, FRAMES + 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
    fixture_t f = fixture(AUDIO_SOUND_PCM16);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_LOOP_END, FRAMES);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_LOOP_START, FRAMES);
    audio_sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t status = open_fixture(&f, NAME, &sound);
    free(f.bytes);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, status);
    TEST_ASSERT_TRUE(sound_empty(&sound));
}

static void
test_one_shot_start(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_LOOP_START, 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
}

static void
test_adpcm_block_frames(void) {
    reject_field(AUDIO_SOUND_IMA_ADPCM, AUDIO_SOUND_AT_BLOCK_FRAMES, 0, sizeof(uint16_t), ASSET_ERR_FORMAT);
    reject_field(AUDIO_SOUND_IMA_ADPCM, AUDIO_SOUND_AT_BLOCK_FRAMES, BLOCK_FRAMES - 1, sizeof(uint16_t),
                 ASSET_ERR_FORMAT);
}

static void
test_loop_alignment(void) {
    fixture_t f = fixture(AUDIO_SOUND_IMA_ADPCM);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_LOOP_START, 1);
    test_pack_put32(f.entry + AUDIO_SOUND_AT_LOOP_END, FRAMES);
    audio_sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t status = open_fixture(&f, NAME, &sound);
    free(f.bytes);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, status);
    TEST_ASSERT_TRUE(sound_empty(&sound));
}

static void
test_pcm_block_frames(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_BLOCK_FRAMES, BLOCK_FRAMES, sizeof(uint16_t), ASSET_ERR_FORMAT);
}

static void
test_block_headers(void) {
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t at = AUDIO_SOUND_HEADER_SIZE + AUDIO_SOUND_ALIGNMENT + i * audio_adpcm_block_bytes(BLOCK_FRAMES);
        reject_field(AUDIO_SOUND_IMA_ADPCM, at + AUDIO_ADPCM_AT_STEP_INDEX, AUDIO_ADPCM_STEP_INDEX_MAX + 1, 1,
                     ASSET_ERR_FORMAT);
        reject_field(AUDIO_SOUND_IMA_ADPCM, at + AUDIO_ADPCM_AT_RESERVED, 1, 1, ASSET_ERR_FORMAT);
    }
}

static void
test_empty(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_FRAMES, 0, sizeof(uint32_t), ASSET_ERR_FORMAT);
}

static void
test_version(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_AT_VERSION, AUDIO_SOUND_VERSION + 1, sizeof(uint16_t),
                 ASSET_ERR_VERSION);
}

static void
test_missing_or_wrong_type(void) {
    fixture_t f = fixture(AUDIO_SOUND_PCM16);
    audio_sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t missing = open_fixture(&f, "absent", &sound);
    const bool missing_cleared = sound_empty(&sound);
    test_pack_put32(f.bytes + ASSET_PACK_HEADER_SIZE + ASSET_NAME_MAX, ASSET_TYPE('N', 'O', 'N', 'E'));
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t wrong_type = open_fixture(&f, NAME, &sound);
    free(f.bytes);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND, missing);
    TEST_ASSERT_TRUE(missing_cleared);
    TEST_ASSERT_EQUAL(ASSET_ERR_TYPE, wrong_type);
    TEST_ASSERT_TRUE(sound_empty(&sound));
}

static void
test_data_gap(void) {
    reject_field(AUDIO_SOUND_PCM16, AUDIO_SOUND_HEADER_SIZE, 1, 1, ASSET_ERR_FORMAT);
}

void
suite_audio_sound(void) {
    RUN_TEST(test_pcm16);
    RUN_TEST(test_adpcm);
    RUN_TEST(test_channels);
    RUN_TEST(test_rate);
    RUN_TEST(test_format);
    RUN_TEST(test_reserved);
    RUN_TEST(test_offset);
    RUN_TEST(test_data_bounds);
    RUN_TEST(test_data_gap);
    RUN_TEST(test_loop_range);
    RUN_TEST(test_one_shot_start);
    RUN_TEST(test_adpcm_block_frames);
    RUN_TEST(test_loop_alignment);
    RUN_TEST(test_pcm_block_frames);
    RUN_TEST(test_block_headers);
    RUN_TEST(test_empty);
    RUN_TEST(test_version);
    RUN_TEST(test_missing_or_wrong_type);
}

SUITE_REGISTER(suite_audio_sound);
