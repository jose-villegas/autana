#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "audio/audio_adpcm.h"
#include "sound/sound.h"
#include "sound/sound_format.h"
#include "suites.h"
#include "test_pack.h"
#include "unity.h"

enum {
    PACK_CAPACITY = 512,
    BLOCK_FRAMES = 9,
    BLOCKS = 3,
    PARTIAL_FRAMES = 2,
    FRAMES = ((BLOCKS - 1) * BLOCK_FRAMES) + PARTIAL_FRAMES,
    CANARY = INT16_MIN,
    SECOND_PREDICTOR = 1000,
    SECOND_INDEX = 10,
    SECOND_SAMPLE = 1002
};

static const char NAME[] = "sound";
static const uint8_t CURSOR_CODES[] = {0x01, 0xa3, 0x5f, 0x82};

typedef struct {
    uint8_t* bytes;
    uint8_t* entry;
    test_pack_t builder;
} fixture_t;

static fixture_t
fixture(sound_format_t format) {
    fixture_t f = {0};
    f.bytes = malloc(PACK_CAPACITY);
    TEST_ASSERT_NOT_NULL(f.bytes);
    f.builder = test_pack_begin(f.bytes, PACK_CAPACITY, 1);
    const uint32_t data_bytes =
        format == SOUND_PCM16 ? FRAMES * SOUND_PCM_BYTES : BLOCKS * audio_adpcm_block_bytes(BLOCK_FRAMES);
    f.entry = test_pack_add(&f.builder, NAME, SOUND_ASSET, SOUND_HEADER_SIZE + SOUND_ALIGNMENT + data_bytes);
    test_pack_put16(f.entry + SOUND_AT_VERSION, SOUND_VERSION);
    f.entry[SOUND_AT_FORMAT] = (uint8_t)format;
    f.entry[SOUND_AT_CHANNELS] = SOUND_CHANNELS;
    test_pack_put32(f.entry + SOUND_AT_SAMPLE_RATE, SOUND_SAMPLE_RATE_HZ);
    test_pack_put32(f.entry + SOUND_AT_FRAMES, FRAMES);
    test_pack_put32(f.entry + SOUND_AT_DATA, SOUND_HEADER_SIZE + SOUND_ALIGNMENT);
    test_pack_put16(f.entry + SOUND_AT_BLOCK_FRAMES, format == SOUND_PCM16 ? 0 : BLOCK_FRAMES);
    return f;
}

static asset_status_t
open_fixture(fixture_t* f, const char* name, sound_t* sound) {
    asset_pack_t pack;
    const uint32_t size = test_pack_finish(&f->builder);
    TEST_ASSERT_EQUAL(ASSET_OK, asset_pack_open(&pack, f->bytes, size));
    return sound_open(&pack, name, sound);
}

static bool
sound_empty(const sound_t* sound) {
    return sound->data == NULL && sound->frames == 0 && sound->loop_start == 0 && sound->loop_end == 0
           && sound->block_frames == 0 && sound->format == 0;
}

static void
reject_fixture(fixture_t* f, asset_status_t expected) {
    sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t status = open_fixture(f, NAME, &sound);
    const bool cleared = sound_empty(&sound);
    free(f->bytes);
    TEST_ASSERT_EQUAL(expected, status);
    TEST_ASSERT_TRUE(cleared);
}

static void
reject_field(sound_format_t format, uint32_t at, uint32_t value, unsigned width, asset_status_t expected) {
    fixture_t f = fixture(format);
    if (width == sizeof(uint32_t)) {
        test_pack_put32(f.entry + at, value);
    } else if (width == sizeof(uint16_t)) {
        test_pack_put16(f.entry + at, (int)value);
    } else {
        f.entry[at] = (uint8_t)value;
    }
    reject_fixture(&f, expected);
}

static void
check_valid(sound_format_t format) {
    fixture_t f = fixture(format);
    const uint32_t start = format == SOUND_PCM16 ? 1 : BLOCK_FRAMES;
    test_pack_put32(f.entry + SOUND_AT_LOOP_START, start);
    test_pack_put32(f.entry + SOUND_AT_LOOP_END, FRAMES);
    sound_t sound;
    memset(&sound, 0xff, sizeof sound);
    const asset_status_t status = open_fixture(&f, NAME, &sound);
    const bool in_place = sound.data == f.entry + SOUND_HEADER_SIZE + SOUND_ALIGNMENT;
    free(f.bytes);
    TEST_ASSERT_EQUAL(ASSET_OK, status);
    TEST_ASSERT_TRUE(in_place);
    TEST_ASSERT_EQUAL(FRAMES, sound.frames);
    TEST_ASSERT_EQUAL(start, sound.loop_start);
    TEST_ASSERT_EQUAL(FRAMES, sound.loop_end);
    TEST_ASSERT_EQUAL(format, sound.format);
    TEST_ASSERT_EQUAL(format == SOUND_PCM16 ? 0 : BLOCK_FRAMES, sound.block_frames);
}

static void
test_pcm16(void) {
    check_valid(SOUND_PCM16);
}

static void
test_adpcm(void) {
    check_valid(SOUND_IMA_ADPCM);
}

static void
test_channels(void) {
    reject_field(SOUND_PCM16, SOUND_AT_CHANNELS, 2, 1, ASSET_ERR_FORMAT);
}

static void
test_rate(void) {
    reject_field(SOUND_PCM16, SOUND_AT_SAMPLE_RATE, SOUND_SAMPLE_RATE_HZ + 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
}

static void
test_format(void) {
    reject_field(SOUND_PCM16, SOUND_AT_FORMAT, SOUND_FORMAT_IMA_ADPCM + 1, 1, ASSET_ERR_FORMAT);
}

static void
test_reserved(void) {
    reject_field(SOUND_PCM16, SOUND_AT_RESERVED16, 1, sizeof(uint16_t), ASSET_ERR_FORMAT);
    reject_field(SOUND_PCM16, SOUND_AT_RESERVED32, 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
    reject_field(SOUND_PCM16, SOUND_AT_RESERVED32 - 1, 1, 1, ASSET_ERR_FORMAT);
    reject_field(SOUND_PCM16, SOUND_AT_DATA - 1, 1, 1, ASSET_ERR_FORMAT);
}

static void
test_offset(void) {
    reject_field(SOUND_PCM16, SOUND_AT_DATA, SOUND_HEADER_SIZE + 1, sizeof(uint32_t), ASSET_ERR_BOUNDS);
    reject_field(SOUND_PCM16, SOUND_AT_DATA, SOUND_HEADER_SIZE - SOUND_ALIGNMENT, sizeof(uint32_t), ASSET_ERR_BOUNDS);
}

static void
test_adpcm_offset(void) {
    reject_field(SOUND_IMA_ADPCM, SOUND_AT_DATA, SOUND_HEADER_SIZE + 1, sizeof(uint32_t), ASSET_ERR_BOUNDS);
    reject_field(SOUND_IMA_ADPCM, SOUND_AT_DATA, SOUND_AT_DATA, sizeof(uint32_t), ASSET_ERR_BOUNDS);
}

static void
test_data_bounds(void) {
    reject_field(SOUND_PCM16, SOUND_AT_FRAMES, UINT32_MAX, sizeof(uint32_t), ASSET_ERR_BOUNDS);
    reject_field(SOUND_IMA_ADPCM, SOUND_AT_FRAMES, UINT32_MAX, sizeof(uint32_t), ASSET_ERR_BOUNDS);
}

static void
test_loop_range(void) {
    reject_field(SOUND_PCM16, SOUND_AT_LOOP_END, FRAMES + 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
    fixture_t f = fixture(SOUND_PCM16);
    test_pack_put32(f.entry + SOUND_AT_LOOP_END, FRAMES);
    test_pack_put32(f.entry + SOUND_AT_LOOP_START, FRAMES);
    reject_fixture(&f, ASSET_ERR_FORMAT);
    f = fixture(SOUND_PCM16);
    test_pack_put32(f.entry + SOUND_AT_LOOP_END, FRAMES - 1);
    test_pack_put32(f.entry + SOUND_AT_LOOP_START, FRAMES);
    reject_fixture(&f, ASSET_ERR_FORMAT);
}

static void
test_one_shot_start(void) {
    reject_field(SOUND_PCM16, SOUND_AT_LOOP_START, 1, sizeof(uint32_t), ASSET_ERR_FORMAT);
}

static void
test_adpcm_block_frames(void) {
    reject_field(SOUND_IMA_ADPCM, SOUND_AT_BLOCK_FRAMES, 0, sizeof(uint16_t), ASSET_ERR_FORMAT);
    reject_field(SOUND_IMA_ADPCM, SOUND_AT_BLOCK_FRAMES, BLOCK_FRAMES - 1, sizeof(uint16_t), ASSET_ERR_FORMAT);
}

static void
test_loop_alignment(void) {
    fixture_t f = fixture(SOUND_IMA_ADPCM);
    test_pack_put32(f.entry + SOUND_AT_LOOP_START, 1);
    test_pack_put32(f.entry + SOUND_AT_LOOP_END, FRAMES);
    reject_fixture(&f, ASSET_ERR_FORMAT);
}

static void
test_pcm_block_frames(void) {
    reject_field(SOUND_PCM16, SOUND_AT_BLOCK_FRAMES, BLOCK_FRAMES, sizeof(uint16_t), ASSET_ERR_FORMAT);
}

static void
test_block_headers(void) {
    for (unsigned i = 0; i < BLOCKS; i++) {
        const uint32_t at = SOUND_HEADER_SIZE + SOUND_ALIGNMENT + i * audio_adpcm_block_bytes(BLOCK_FRAMES);
        reject_field(SOUND_IMA_ADPCM, at + AUDIO_ADPCM_AT_STEP_INDEX, AUDIO_ADPCM_STEP_INDEX_MAX + 1, 1,
                     ASSET_ERR_FORMAT);
        reject_field(SOUND_IMA_ADPCM, at + AUDIO_ADPCM_AT_RESERVED, 1, 1, ASSET_ERR_FORMAT);
    }
}

static void
test_empty(void) {
    reject_field(SOUND_PCM16, SOUND_AT_FRAMES, 0, sizeof(uint32_t), ASSET_ERR_FORMAT);
}

static void
test_version(void) {
    reject_field(SOUND_PCM16, SOUND_AT_VERSION, SOUND_VERSION + 1, sizeof(uint16_t), ASSET_ERR_VERSION);
}

static void
test_missing_or_wrong_type(void) {
    fixture_t f = fixture(SOUND_PCM16);
    sound_t sound;
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
    reject_field(SOUND_PCM16, SOUND_HEADER_SIZE, 1, 1, ASSET_ERR_FORMAT);
    reject_field(SOUND_PCM16, SOUND_HEADER_SIZE + SOUND_ALIGNMENT - 1, 1, 1, ASSET_ERR_FORMAT);
}

static void
check_cursor(sound_format_t format) {
    fixture_t f = fixture(format);
    const uint32_t start = format == SOUND_IMA_ADPCM ? BLOCK_FRAMES : PARTIAL_FRAMES;
    uint8_t* data = f.entry + SOUND_HEADER_SIZE + SOUND_ALIGNMENT;
    if (format == SOUND_PCM16) {
        for (unsigned i = 0; i < FRAMES; i++) {
            test_pack_put16(data + i * SOUND_PCM_BYTES, -(int)i);
        }
    } else {
        test_pack_put16(data, -CANARY / 2);
        memcpy(data + AUDIO_ADPCM_BLOCK_HEADER_BYTES, CURSOR_CODES, sizeof CURSOR_CODES);
        uint8_t* second = data + audio_adpcm_block_bytes(BLOCK_FRAMES);
        test_pack_put16(second, SECOND_PREDICTOR);
        second[AUDIO_ADPCM_AT_STEP_INDEX] = SECOND_INDEX;
    }
    sound_t sound;
    TEST_ASSERT_EQUAL(ASSET_OK, open_fixture(&f, NAME, &sound));
    sound_cursor_t cursor;
    int16_t whole[FRAMES + 1], split[FRAMES + 1];
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, 0));
    whole[FRAMES] = CANARY;
    TEST_ASSERT_EQUAL(FRAMES, sound_read(&cursor, whole, FRAMES + 1));
    TEST_ASSERT_EQUAL(CANARY, whole[FRAMES]);
    TEST_ASSERT_EQUAL(FRAMES, cursor.position);
    TEST_ASSERT_EQUAL(0, sound_read(&cursor, whole + FRAMES, 1));
    TEST_ASSERT_EQUAL(CANARY, whole[FRAMES]);
    if (format == SOUND_PCM16) {
        for (unsigned i = 0; i < FRAMES; i++) {
            TEST_ASSERT_EQUAL(sound_read_sample(data + i * SOUND_PCM_BYTES), whole[i]);
        }
    }
    if (format == SOUND_IMA_ADPCM) {
        /* IMA code zero at index 10 adds 19 >> 3 to the predictor. */
        TEST_ASSERT_EQUAL(SECOND_PREDICTOR, whole[BLOCK_FRAMES]);
        TEST_ASSERT_EQUAL(SECOND_SAMPLE, whole[BLOCK_FRAMES + 1]);
    }
    /* Both nibble parities and block edges must survive a read boundary. */
    for (uint32_t first = 1; first < FRAMES; first++) {
        TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, 0));
        split[0] = CANARY;
        TEST_ASSERT_EQUAL(0, sound_read(&cursor, split, 0));
        TEST_ASSERT_EQUAL(CANARY, split[0]);
        split[first] = CANARY;
        TEST_ASSERT_EQUAL(first, sound_read(&cursor, split, first));
        TEST_ASSERT_EQUAL(CANARY, split[first]);
        TEST_ASSERT_EQUAL(first, cursor.position);
        split[FRAMES] = CANARY;
        TEST_ASSERT_EQUAL(FRAMES - first, sound_read(&cursor, split + first, FRAMES));
        TEST_ASSERT_EQUAL(CANARY, split[FRAMES]);
        TEST_ASSERT_EQUAL_INT16_ARRAY(whole, split, FRAMES);
    }
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, BLOCK_FRAMES));
    split[FRAMES - BLOCK_FRAMES] = CANARY;
    TEST_ASSERT_EQUAL(FRAMES - BLOCK_FRAMES, sound_read(&cursor, split, FRAMES));
    TEST_ASSERT_EQUAL(CANARY, split[FRAMES - BLOCK_FRAMES]);
    TEST_ASSERT_EQUAL_INT16_ARRAY(whole + BLOCK_FRAMES, split, FRAMES - BLOCK_FRAMES);
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, start));
    TEST_ASSERT_EQUAL(start, cursor.position);
    split[FRAMES - start] = CANARY;
    TEST_ASSERT_EQUAL(FRAMES - start, sound_read(&cursor, split, FRAMES));
    TEST_ASSERT_EQUAL(CANARY, split[FRAMES - start]);
    TEST_ASSERT_EQUAL_INT16_ARRAY(whole + start, split, FRAMES - start);
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, FRAMES));
    split[0] = CANARY;
    TEST_ASSERT_EQUAL(0, sound_read(&cursor, split, FRAMES));
    TEST_ASSERT_EQUAL(CANARY, split[0]);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, sound_seek(&cursor, &sound, FRAMES + 1));
    TEST_ASSERT_NULL(cursor.sound);
    TEST_ASSERT_EQUAL(0, cursor.position);
    TEST_ASSERT_NULL(cursor.adpcm.codes);
    for (uint32_t frame = (BLOCK_FRAMES * 2) - 1; frame <= (BLOCK_FRAMES * 2) + 1; frame++) {
        memset(&cursor, 0xff, sizeof cursor);
        const bool rejected = format == SOUND_IMA_ADPCM && frame != BLOCK_FRAMES * 2;
        TEST_ASSERT_EQUAL(rejected ? ASSET_ERR_FORMAT : ASSET_OK, sound_seek(&cursor, &sound, frame));
        if (rejected) {
            TEST_ASSERT_NULL(cursor.sound);
            TEST_ASSERT_EQUAL(0, cursor.position);
            TEST_ASSERT_NULL(cursor.adpcm.codes);
            TEST_ASSERT_EQUAL(0, cursor.adpcm.predictor);
            TEST_ASSERT_EQUAL(0, cursor.adpcm.step_index);
            TEST_ASSERT_EQUAL(0, cursor.adpcm.pending);
            TEST_ASSERT_EQUAL(0, sound_read(&cursor, split, FRAMES));
        }
    }
    free(f.bytes);
}

static void
check_loop(sound_format_t format, uint32_t start, uint32_t end) {
    fixture_t f = fixture(format);
    uint8_t* data = f.entry + SOUND_HEADER_SIZE + SOUND_ALIGNMENT;
    if (format == SOUND_PCM16) {
        for (unsigned i = 0; i < FRAMES; i++) {
            test_pack_put16(data + (i * SOUND_PCM_BYTES), -(int)i);
        }
    } else {
        memcpy(data + AUDIO_ADPCM_BLOCK_HEADER_BYTES, CURSOR_CODES, sizeof CURSOR_CODES);
        uint8_t* second = data + audio_adpcm_block_bytes(BLOCK_FRAMES);
        test_pack_put16(second, SECOND_PREDICTOR);
        second[AUDIO_ADPCM_AT_STEP_INDEX] = SECOND_INDEX;
    }
    sound_t sound;
    sound_cursor_t cursor;
    int16_t source[FRAMES];

    enum { READ_FRAMES = (FRAMES * 3) + 1, CHUNK_FRAMES = 3 };

    int16_t expected[READ_FRAMES], actual[READ_FRAMES];
    TEST_ASSERT_EQUAL(ASSET_OK, open_fixture(&f, NAME, &sound));
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, 0));
    TEST_ASSERT_EQUAL(FRAMES, sound_read(&cursor, source, FRAMES));
    for (unsigned i = 0; i < READ_FRAMES; i++) {
        const unsigned frame = i < end ? i : start + ((i - end) % (end - start));
        expected[i] = source[frame];
    }
    test_pack_put32(f.entry + SOUND_AT_LOOP_START, start);
    test_pack_put32(f.entry + SOUND_AT_LOOP_END, end);
    TEST_ASSERT_EQUAL(ASSET_OK, open_fixture(&f, NAME, &sound));
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, 0));
    TEST_ASSERT_EQUAL(READ_FRAMES, sound_read(&cursor, actual, READ_FRAMES));
    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, actual, READ_FRAMES);
    TEST_ASSERT_EQUAL(ASSET_OK, sound_seek(&cursor, &sound, 0));
    for (unsigned i = 0; i < READ_FRAMES;) {
        const unsigned left = READ_FRAMES - i;
        const unsigned count = left < CHUNK_FRAMES ? left : CHUNK_FRAMES;
        TEST_ASSERT_EQUAL(count, sound_read(&cursor, actual + i, count));
        i += count;
    }
    TEST_ASSERT_EQUAL_INT16_ARRAY(expected, actual, READ_FRAMES);
    free(f.bytes);
}

static void
test_loop_cursors(void) {
    for (unsigned format = SOUND_PCM16; format <= SOUND_IMA_ADPCM; format++) {
        check_loop((sound_format_t)format, BLOCK_FRAMES, FRAMES - 1);
        check_loop((sound_format_t)format, 0, FRAMES);
    }
}

static void
test_pcm_cursor(void) {
    check_cursor(SOUND_PCM16);
}

static void
test_adpcm_cursor(void) {
    check_cursor(SOUND_IMA_ADPCM);
}

static void
test_exact_fit_minus_one(void) {
    for (unsigned format = SOUND_PCM16; format <= SOUND_IMA_ADPCM; format++) {
        fixture_t f = fixture((sound_format_t)format);
        /* The directory size, not the aligned pack size, bounds an entry. */
        uint8_t* size = f.bytes + ASSET_PACK_HEADER_SIZE + ASSET_NAME_MAX + sizeof(uint32_t) * 2;
        test_pack_put32(size, asset_read_u32(size) - 1);
        reject_fixture(&f, ASSET_ERR_BOUNDS);
    }
}

static void
test_step_index_max_and_full_loop(void) {
    fixture_t f = fixture(SOUND_IMA_ADPCM);
    f.entry[SOUND_HEADER_SIZE + SOUND_ALIGNMENT + AUDIO_ADPCM_AT_STEP_INDEX] = AUDIO_ADPCM_STEP_INDEX_MAX;
    test_pack_put32(f.entry + SOUND_AT_LOOP_END, FRAMES);
    sound_t sound;
    TEST_ASSERT_EQUAL(ASSET_OK, open_fixture(&f, NAME, &sound));
    TEST_ASSERT_EQUAL(0, sound.loop_start);
    TEST_ASSERT_EQUAL(FRAMES, sound.loop_end);
    free(f.bytes);
}

void
suite_sound(void) {
    RUN_TEST(test_loop_cursors);
    RUN_TEST(test_pcm_cursor);
    RUN_TEST(test_adpcm_cursor);
    RUN_TEST(test_exact_fit_minus_one);
    RUN_TEST(test_step_index_max_and_full_loop);
    RUN_TEST(test_pcm16);
    RUN_TEST(test_adpcm);
    RUN_TEST(test_channels);
    RUN_TEST(test_rate);
    RUN_TEST(test_format);
    RUN_TEST(test_reserved);
    RUN_TEST(test_offset);
    RUN_TEST(test_adpcm_offset);
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

SUITE_REGISTER(suite_sound);
