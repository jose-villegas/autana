/* input_motion: frame cache for the shared gravity and shake readers. */
#pragma once
#include <stdint.h>

/* Called once per frame by input_poll; no sensor work until a reader asks. */
void input_motion_frame(int64_t now_us);
/* Driver start and isolated host fixtures only; app switches retain state. */
void input_motion_reset(void);
