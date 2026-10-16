/* Acoustic tone detection against a same-frequency microphone baseline. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define AUDIO_LOOPBACK_SETTLE_MS       100u
#define AUDIO_LOOPBACK_HEARD_MARGIN_DB 20.0f

typedef struct {
    float tone_db;
    float silence_db;
    bool heard;
} audio_loopback_t;

/* Blocking; call from a worker or console task, never the frame loop. */
bool audio_loopback_check(uint16_t hz, uint16_t ms, uint8_t volume_pct, audio_loopback_t* result);
