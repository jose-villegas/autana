/* console_panelcap_device: panel recording within the current app heap visit. */
#pragma once

#include "console/console.h"
#include "gfx/present/capture/panelcap.h"

#define PANELCAP_LINE_BYTES 128U
#define PANELCAP_CAPTURE_BYTES                                                                                         \
    (PANELCAP_IMAGE_BYTES + sizeof(panelcap_encoder_t) + CONSOLE_PACKET_BYTES_MAX + CONSOLE_PACKET_MAX)
#define PANELCAP_ERROR_PREFIX "PANELCAP ERR "
#define PANELCAP_APP_SWITCH   "PANELCAP STOP app switch"
#define PANELCAP_NO_MEMORY    PANELCAP_ERROR_PREFIX "no memory: need %zu, largest free %zu"

void gfx_panel_capture_start(unsigned fps, console_reply_fn reply);
void gfx_panel_capture_stop(void);
void gfx_panel_capture_end_visit(void);
void gfx_panel_capture_leave_launcher(void);
