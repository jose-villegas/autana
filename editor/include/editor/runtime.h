/* Host toolkit initialization, geometry and UI constraints. */
#ifndef EDITOR_RUNTIME_H
#define EDITOR_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool editor_runtime_init(void);
int editor_runtime_panel_width(void);
int editor_runtime_panel_height(void);
int editor_runtime_tap_min(void);

#ifdef __cplusplus
}
#endif

#endif
