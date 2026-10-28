#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "asset/asset_bytes.h"
#include "asset/asset_store.h"
#include "audio/audio_adpcm.h"
#include "math/scalar/rng.h"
#include "sound/sound.h"
#include "sound/sound_format.h"
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
    const handle_t load = asset_store_load("engine");
    const asset_pack_t* pack = asset_store_loaded(load);
    TEST_ASSERT_NOT_NULL(pack);
    sound_t sound;
    const asset_status_t status = sound_open(pack, "tap", &sound);
    asset_store_release(load);
    TEST_ASSERT_EQUAL(ASSET_OK, status);
    TEST_ASSERT_EQUAL(SOUND_IMA_ADPCM, sound.format);
    TEST_ASSERT_EQUAL(SOUND_DEFAULT_BLOCK_FRAMES, sound.block_frames);
    TEST_ASSERT_GREATER_THAN(0, sound.frames);
    TEST_ASSERT_EQUAL(0, sound.loop_end);
}

static int
oracle_step(int* predictor, int* index, unsigned code) {
    static const int16_t steps[] = AUDIO_ADPCM_STEP_TABLE;
    static const int8_t indices[] = AUDIO_ADPCM_INDEX_TABLE;
    const int magnitude = steps[*index];
    int diff = magnitude >> 3;
    if (code & 1U) {
        diff += magnitude >> 2;
    }
    if (code & 2U) {
        diff += magnitude >> 1;
    }
    if (code & 4U) {
        diff += magnitude;
    }
    *predictor += code & AUDIO_ADPCM_SIGN_BIT ? -diff : diff;
    if (*predictor > INT16_MAX) {
        *predictor = INT16_MAX;
    }
    if (*predictor < INT16_MIN) {
        *predictor = INT16_MIN;
    }
    *index += indices[code];
    if (*index < 0) {
        *index = 0;
    }
    if (*index > (int)AUDIO_ADPCM_STEP_INDEX_MAX) {
        *index = AUDIO_ADPCM_STEP_INDEX_MAX;
    }
    return *predictor;
}

static void
test_all_adpcm_steps(void) {
    static const int predictors[] = {INT16_MIN, INT16_MIN + 1, 0, INT16_MAX - 1, INT16_MAX};
    for (unsigned i = 0; i <= AUDIO_ADPCM_STEP_INDEX_MAX; i++) {
        for (unsigned code = 0; code <= AUDIO_ADPCM_CODE_MASK; code++) {
            for (unsigned p = 0; p < sizeof predictors / sizeof predictors[0]; p++) {
                uint8_t packed = (uint8_t)code;
                audio_adpcm_t state = {.codes = &packed, .predictor = predictors[p], .step_index = (int8_t)i};
                int predictor = predictors[p], index = (int)i;
                const int expected = oracle_step(&predictor, &index, code);
                int16_t actual;
                audio_adpcm_read(&state, &actual, 1);
                TEST_ASSERT_EQUAL_INT16(expected, actual);
                TEST_ASSERT_EQUAL_INT(index, state.step_index);
            }
        }
    }
}

static void
test_random_adpcm_chunks(void) {
    enum { BYTES = 4096, MAX_CHUNK = 131, SEEDS = 4 };

    uint8_t* codes = malloc(BYTES);
    int16_t* out = malloc(MAX_CHUNK * sizeof *out);
    TEST_ASSERT_NOT_NULL(codes);
    TEST_ASSERT_NOT_NULL(out);
    for (unsigned seed = 1; seed <= SEEDS; seed++) {
        rng_t random;
        rng_seed(&random, seed);
        for (unsigned i = 0; i < BYTES; i++) {
            codes[i] = (uint8_t)rng_next(&random);
        }
        audio_adpcm_t state = {.codes = codes};
        int predictor = 0, index = 0;
        for (unsigned at = 0; at < BYTES * AUDIO_ADPCM_CODES_PER_BYTE;) {
            unsigned count = 1 + at % MAX_CHUNK;
            if (count > BYTES * AUDIO_ADPCM_CODES_PER_BYTE - at) {
                count = BYTES * AUDIO_ADPCM_CODES_PER_BYTE - at;
            }
            audio_adpcm_read(&state, out, count);
            for (unsigned j = 0; j < count; j++) {
                unsigned pos = at + j;
                unsigned code = (codes[pos / AUDIO_ADPCM_CODES_PER_BYTE]
                                 >> ((pos % AUDIO_ADPCM_CODES_PER_BYTE) * AUDIO_ADPCM_CODE_BITS))
                                & AUDIO_ADPCM_CODE_MASK;
                TEST_ASSERT_EQUAL_INT16(oracle_step(&predictor, &index, code), out[j]);
            }
            at += count;
            TEST_ASSERT_EQUAL_INT(predictor, state.predictor);
            TEST_ASSERT_EQUAL_INT(index, state.step_index);
            TEST_ASSERT_EQUAL_PTR(codes + (at + 1) / AUDIO_ADPCM_CODES_PER_BYTE, state.codes);
        }
    }
    free(out);
    free(codes);
}

void
suite_audio_adpcm(void) {
    RUN_TEST(test_all_adpcm_steps);
    RUN_TEST(test_random_adpcm_chunks);
    RUN_TEST(test_ffmpeg_reference);
    RUN_TEST(test_ffmpeg_saturation_reference);
#ifndef DEVICE_BUILD
    RUN_TEST(test_baker_multiblock_round_trip);
#endif
    RUN_TEST(test_engine_sound);
}

SUITE_REGISTER(suite_audio_adpcm);
SUITE_READS(suite_audio_adpcm, "engine");
