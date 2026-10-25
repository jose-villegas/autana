/* motion_reader: frame cache for the shared gravity and shake readers. */
#pragma once
#include <stdint.h>

/* Called once per frame by input_poll; no sensor work until a reader asks. */
void motion_reader_frame(int64_t now_us);
/* Driver start and isolated host fixtures only; app switches retain state. */
void motion_reader_reset(void);
