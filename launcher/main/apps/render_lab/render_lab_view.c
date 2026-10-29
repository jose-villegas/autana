#include "apps/render_lab/render_lab_view.h"

#include <string.h>

#include "core/tune.h"

TUNE_OWNER(render_lab);
TUNE(render_lab, view, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_COUNT);
TUNE(render_lab, scale, RENDER_LAB_SCALE_DEFAULT, RENDER_LAB_SCALE_UNIT, RENDER_LAB_SCALE_MAX);
TUNE(render_lab, present, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_DOUBLE);
TUNE_SETTING(render_lab, small_fill_ceiling, SMALL_FILL_CEILING_SMOOTH, 0, SMALL_FILL_CEILING_MAX);
TUNE_SETTING(render_lab, small_fill_gate, SMALL_FILL_GATE_SMOOTH, 0, UINT8_MAX);
TUNE_SETTING(render_lab, small_fill_style, SMALL_FILL_FLAT, SMALL_FILL_FLAT, SMALL_FILL_STYLE_COUNT - 1);
TUNE_SETTING(render_lab, budget, 0, 0, RENDER_LAB_BUDGET_MAX_MS);
TUNE(render_lab, layout, RENDER_LAB_LAYOUT_FULL, RENDER_LAB_LAYOUT_FULL, RENDER_LAB_LAYOUT_COUNT - 1);
TUNE(render_lab, orbit_ms, 0, 0, RENDER_LAB_ORBIT_MAX_MS);
TUNE_SETTING(render_lab, framegen, 0, 0, 1);

int
render_lab_debug_view(void) {
    return (int)view;
}

int
render_lab_scale(void) {
    return scale;
}

camera_present_t
render_lab_present(void) {
    return (camera_present_t)present;
}

render_lab_layout_t
render_lab_layout(void) {
    return (render_lab_layout_t)layout;
}

small_fill_t
render_lab_small_fill(void) {
    return (small_fill_t){(uint8_t)small_fill_ceiling, (uint8_t)small_fill_gate, (small_fill_style_t)small_fill_style};
}

#define SMALL_FILL_PRESET_ROW(name)   SMALL_FILL_##name,
#define SMALL_FILL_PRESET_NAME(name)  #name,
/* A menu label is the preset name with each '_' shown as the separator; its buffer fits a name of only '_'. */
#define SMALL_FILL_LABEL_SEPARATOR    " - "
#define SMALL_FILL_PRESET_LABEL(name) char name[sizeof #name * (sizeof SMALL_FILL_LABEL_SEPARATOR - 1)];
static const small_fill_t small_fill_presets[] = {SMALL_FILL_PRESETS(SMALL_FILL_PRESET_ROW)};
static const char* const small_fill_preset_names[] = {SMALL_FILL_PRESETS(SMALL_FILL_PRESET_NAME)};
#define SMALL_FILL_PRESET_COUNT ((int)(sizeof small_fill_presets / sizeof small_fill_presets[0]))
static char small_fill_labels[SMALL_FILL_PRESET_COUNT][sizeof(union {SMALL_FILL_PRESETS(SMALL_FILL_PRESET_LABEL)})];
static const char* small_fill_names[SMALL_FILL_PRESET_COUNT];
static int small_fill_picked;

const char* const*
render_lab_small_fill_names(int* count) {
    for (int i = 0; i < SMALL_FILL_PRESET_COUNT && small_fill_names[i] == NULL; i++) {
        char* label = small_fill_labels[i];
        for (const char* c = small_fill_preset_names[i]; *c != '\0'; c++) {
            if (*c == '_') {
                memcpy(label, SMALL_FILL_LABEL_SEPARATOR, sizeof SMALL_FILL_LABEL_SEPARATOR - 1);
                label += sizeof SMALL_FILL_LABEL_SEPARATOR - 1;
            } else {
                *label++ = *c;
            }
        }
        small_fill_names[i] = small_fill_labels[i];
    }
    *count = SMALL_FILL_PRESET_COUNT;
    return (const char* const*)small_fill_names;
}

int
render_lab_small_fill_selected(void) {
    return small_fill_picked;
}

bool
render_lab_small_fill_select(int index) {
    if (index < 0 || index >= SMALL_FILL_PRESET_COUNT) {
        return false;
    }
    small_fill_picked = index;
    const small_fill_t fill = small_fill_presets[index];
    return TUNE_SETTING_SET(small_fill_ceiling, fill.ceiling) && TUNE_SETTING_SET(small_fill_gate, fill.gate)
           && TUNE_SETTING_SET(small_fill_style, fill.style);
}

bool
render_lab_apply_camera_settings(scene_t* scene, scene_entity_t camera) {
    return scene_camera_set_present(scene, camera, render_lab_present())
           && scene_camera_set_small_fill(scene, camera, render_lab_small_fill());
}

resolution_step_t
render_lab_render_size(int width, int height) {
    const int percent = RENDER_LAB_SCALE_UNIT * RENDER_LAB_SCALE_UNIT / render_lab_scale();
    return raster_present_size(
        (resolution_step_t){width * percent / RENDER_LAB_SCALE_UNIT, height * percent / RENDER_LAB_SCALE_UNIT},
        render_lab_present());
}

void
render_lab_apply_raster_settings(void) {
    render_context_set_scale(render_context_main(), RENDER_LAB_SCALE_UNIT * RENDER_LAB_SCALE_UNIT / render_lab_scale());
    render_context_set_frame_generation(render_context_main(), render_lab_framegen_on());
}

bool
render_lab_framegen_on(void) {
    return framegen != 0;
}

bool
render_lab_framegen_set_on(bool on) {
    return TUNE_SETTING_SET(framegen, on ? 1 : 0);
}

int
render_lab_budget_ms(void) {
    return budget;
}

bool
render_lab_budget_set_ms(int budget_ms) {
    return TUNE_SETTING_SET(budget, budget_ms);
}

int
render_lab_orbit_ms(void) {
    return orbit_ms;
}
