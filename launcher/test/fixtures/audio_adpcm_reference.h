/* Input in.raw is LE int16: 0,1000,-1000,2000,-2000,3000,-3000,32767,-32768.
 * ffmpeg -f s16le -ar 22050 -ac 1 -i in.raw -c:a adpcm_ima_wav -block_size 32 out.wav
 * ffmpeg -i out.wav -f s16le decoded.raw
 * BLOCK is out.wav's data chunk; EXPECTED is decoded.raw, including padding. */
#pragma once

#include <stdint.h>

static const uint8_t BLOCK[] = {
    0x00, 0x00, 0x00, 0x00, 0xf7, 0xf7, 0xf7, 0xf7, 0x82, 0x80, 0x80, 0x80, 0x00, 0x88, 0x00, 0x88,
    0x00, 0x88, 0x00, 0x88, 0x00, 0x88, 0x80, 0x00, 0x08, 0x88, 0x00, 0x88, 0x00, 0x88, 0x80, 0x80,
};
static const int16_t EXPECTED[] = {
    0,   11,   -19, 44,  -92, 201, -430, 927, -1983, 95,  -283, 60, -252, 32,  -226, 8,  -205, -11, 165,
    5,   -140, -8,  112, 3,   -96, -6,   76,  2,     -66, -5,   51, 0,    -46, -4,   34, 0,    -31, -3,
    -29, -6,   15,  -4,  13,  -3,  -17,  -4,  8,     -3,  -13,  -4, 4,    -3,  -9,   -3, -8,   -3,  -7,
};
