/* Host-only gfx test support. */
#pragma once

#ifndef ESP_PLATFORM
void gfx_reset_for_test(void);
unsigned gfx_fb_guard_trips_for_test(void);
#endif
