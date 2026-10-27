/* Frame-task access to mapped console sound views and their voice handles. */
#pragma once

#include "sound/console/console_sound.h"
#include "sound/sound.h"
#include "sound/sound_play.h"

void console_sound_track_voice(const sound_t* sound, sound_voice_t voice);
const sound_t* console_sound_view(const console_sound_args_t* args, int64_t* open_us);
