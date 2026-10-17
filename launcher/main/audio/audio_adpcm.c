#include "audio/audio_adpcm.h"

#include <limits.h>
#include "asset/asset_bytes.h"

static const int STEP[] = AUDIO_ADPCM_STEP_TABLE;
static const int INDEX[] = AUDIO_ADPCM_INDEX_TABLE;

void
audio_adpcm_decode_block(const uint8_t* block, uint32_t frames, int16_t* out) {
    const uint16_t stored = asset_read_u16(block);
    int predictor = stored <= INT16_MAX ? (int)stored : (int)stored - (UINT16_MAX + 1);
    int index = block[AUDIO_ADPCM_AT_STEP_INDEX];
    out[0] = (int16_t)predictor;
    for (uint32_t i = 1; i < frames; i++) {
        const unsigned shift = ((i - 1U) & 1U) * AUDIO_ADPCM_CODE_BITS;
        const unsigned code =
            (block[AUDIO_ADPCM_BLOCK_HEADER_BYTES + ((i - 1U) >> 1U)] >> shift) & AUDIO_ADPCM_CODE_MASK;
        const int step = STEP[index];
        int diff = step >> 3;
        if (code & 1U) {
            diff += step >> 2;
        }
        if (code & 2U) {
            diff += step >> 1;
        }
        if (code & 4U) {
            diff += step;
        }
        predictor += code & AUDIO_ADPCM_SIGN_BIT ? -diff : diff;
        predictor = predictor < INT16_MIN ? INT16_MIN : predictor > INT16_MAX ? INT16_MAX : predictor;
        index += INDEX[code];
        index = index<0 ? 0 : index>(int) AUDIO_ADPCM_STEP_INDEX_MAX ? (int)AUDIO_ADPCM_STEP_INDEX_MAX : index;
        out[i] = (int16_t)predictor;
    }
}
