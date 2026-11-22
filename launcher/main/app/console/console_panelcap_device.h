/* console_panelcap_device: panel recording within the current app heap visit. */
#pragma once

#include "console/console.h"
#include "gfx/present/capture/panelcap.h"

#define PANELCAP_LINE_BYTES 144U
#define PANELCAP_CAPTURE_BYTES                                                                                         \
    (PANELCAP_IMAGE_BYTES + sizeof(panelcap_encoder_t) + CONSOLE_PACKET_BYTES_MAX + CONSOLE_PACKET_MAX)
#define PANELCAP_PACKET_TAG   "PANEL"
#define PANELCAP_ERROR_PREFIX "PANELCAP ERR "
#define PANELCAP_APP_SWITCH   "PANELCAP STOP app switch"
#define PANELCAP_NO_MEMORY    PANELCAP_ERROR_PREFIX "no memory: need %zu, largest free %zu"
#define PANELCAP_NO_SENDER    "PANELCAP_REFUSED: no memory for sender"

void console_panelcap_start(unsigned fps, console_reply_fn reply);
void console_panelcap_stop(void);
void console_panelcap_end_visit(void);
void console_panelcap_leave_launcher(void);
