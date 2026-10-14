/* Host-only gfx test support. */
#pragma once

#ifndef ESP_PLATFORM
/* Treat the GFX_BAND_HEIGHT framebuffer rows from row as the band being drawn. */
void gfx_band_begin_for_test(int row);
/* End the band being drawn. */
void gfx_band_end_for_test(void);
void gfx_reset_for_test(void);
unsigned gfx_fb_guard_trips_for_test(void);
#endif
