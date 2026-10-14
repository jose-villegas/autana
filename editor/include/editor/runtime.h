/* Panel geometry and UI constraints from firmware headers. */
#ifndef EDITOR_RUNTIME_H
#define EDITOR_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

int editor_runtime_panel_width(void);
int editor_runtime_panel_height(void);
int editor_runtime_tap_min(void);

#ifdef __cplusplus
}
#endif

#endif
