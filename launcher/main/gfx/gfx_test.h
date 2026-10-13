/* Host-only gfx test support. */
#pragma once

#ifndef ESP_PLATFORM
/* Draw into the full framebuffer's band at row. */
void gfx_band_begin_for_test(int row);
/* End the simulated band replay. */
void gfx_band_end_for_test(void);
void gfx_reset_for_test(void);
unsigned gfx_fb_guard_trips_for_test(void);
#endif
