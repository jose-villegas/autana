/* console_tone: blocking tone control and loopback checks for console workers. */
#pragma once
#include "sound/console/console_tone_reply.h"
#include "sound/sound_engine.h"

bool console_tone_start(sound_voice_t* voice, uint16_t hz, uint16_t ms, uint8_t gain);
bool console_tone_stop(sound_voice_t* voice);
/* NULL on success; otherwise a reason suitable for the caller's error prefix. */
const char* console_tone_check(const console_tone_args_t* args, char result[CONSOLE_TONE_REPLY_BYTES]);
