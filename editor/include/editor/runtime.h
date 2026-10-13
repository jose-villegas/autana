/* Host access to firmware rendering, geometry and UI constraints. */
#ifndef EDITOR_RUNTIME_H
#define EDITOR_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EDITOR_SCREEN_LAUNCHER,
    EDITOR_SCREEN_COUNT,
} editor_screen_t;

bool editor_runtime_init(void);
int editor_runtime_panel_width(void);
int editor_runtime_panel_height(void);
int editor_runtime_tap_min(void);

/* Renders `screen` through the firmware's own UI and gfx code into a
 * width x height buffer of native-endian RGB565, 368 x 448 or 448 x 368. */
bool editor_runtime_render(editor_screen_t screen, uint16_t* pixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
