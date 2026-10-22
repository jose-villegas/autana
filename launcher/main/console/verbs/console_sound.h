/* Frame-task ownership of console sound views and their mounted packs. */
#pragma once

#include <stdbool.h>

/* Answers a play request taken from the frame mailbox. */
void console_sound_answer(const char* args, bool app_owned);
/* After sound_stop_app_voices(): releases views borrowed by the exiting app. */
void console_sound_app_exit(void);
