/* Portable IMA-ADPCM block decoding into caller-owned PCM16 samples. */
#pragma once

#include <stdint.h>

#define AUDIO_ADPCM_BLOCK_HEADER_BYTES 4U
#define AUDIO_ADPCM_STEP_INDEX_MAX     88U
#define AUDIO_ADPCM_AT_STEP_INDEX      2U
#define AUDIO_ADPCM_AT_RESERVED        3U
#define AUDIO_ADPCM_CODES_PER_BYTE     2U
#define AUDIO_ADPCM_CODE_BITS          4U
#define AUDIO_ADPCM_CODE_MASK          15U
#define AUDIO_ADPCM_SIGN_BIT           8U

static inline uint32_t
audio_adpcm_block_bytes(uint32_t block_frames) {
    return AUDIO_ADPCM_BLOCK_HEADER_BYTES + (block_frames - 1U) / AUDIO_ADPCM_CODES_PER_BYTE;
}

/* block has passed audio_sound_open; frames is in 1..block_frames and out holds frames samples. */
void audio_adpcm_decode_block(const uint8_t* block, uint32_t frames, int16_t* out);

// clang-format off
#define AUDIO_ADPCM_STEP_TABLE { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 }
#define AUDIO_ADPCM_INDEX_TABLE {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}
// clang-format on
