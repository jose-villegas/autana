#include "apps/render_lab/render_lab_view.h"

#include "services/tune.h"

TUNE_OWNER(render_lab);
TUNE(render_lab, view, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_SHADED, RENDER_DEBUG_VIEW_COUNT);
TUNE(render_lab, scale, RENDER_LAB_SCALE_DEFAULT, RENDER_LAB_SCALE_UNIT, RENDER_LAB_SCALE_MAX);
TUNE(render_lab, present, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_DOUBLE);
TUNE(render_lab, small_fill_ceiling, SMALL_FILL_CEILING_SMOOTH, 0, SMALL_FILL_CEILING_MAX);
TUNE(render_lab, small_fill_gate, SMALL_FILL_GATE_SMOOTH, 0, UINT8_MAX);
TUNE(render_lab, small_fill_style, SMALL_FILL_FLAT, SMALL_FILL_FLAT, SMALL_FILL_STYLE_COUNT - 1);
TUNE(render_lab, budget, 0, 0, 200);

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

small_fill_t
render_lab_small_fill(void) {
    return (small_fill_t){(uint8_t)small_fill_ceiling, (uint8_t)small_fill_gate, (small_fill_style_t)small_fill_style};
}

typedef struct {
    const char* name;
    small_fill_t fill;
} small_fill_preset_t;

#define SMALL_FILL_PRESET_ROW(name) {#name, SMALL_FILL_##name},
static const small_fill_preset_t small_fill_presets[] = {SMALL_FILL_PRESETS(SMALL_FILL_PRESET_ROW)};
#define SMALL_FILL_PRESET_COUNT      ((int)(sizeof small_fill_presets / sizeof small_fill_presets[0]))
#define SMALL_FILL_PRESET_NAME(name) #name,
static const char* const small_fill_names[] = {SMALL_FILL_PRESETS(SMALL_FILL_PRESET_NAME) "CUSTOM"};

const char* const*
render_lab_small_fill_names(int* count) {
    *count = SMALL_FILL_PRESET_COUNT + 1;
    return small_fill_names;
}

int
render_lab_small_fill_selected(void) {
    const small_fill_t now = render_lab_small_fill();
    for (int i = 0; i < SMALL_FILL_PRESET_COUNT; i++) {
        const small_fill_t fill = small_fill_presets[i].fill;
        if (fill.ceiling == now.ceiling && fill.gate == now.gate && fill.style == now.style) {
            return i;
        }
    }
    return SMALL_FILL_PRESET_COUNT;
}

void
render_lab_small_fill_select(int index) {
#if TUNE_ENABLED
    if (index < 0 || index >= SMALL_FILL_PRESET_COUNT) {
        return;
    }
    const small_fill_t fill = small_fill_presets[index].fill;
    tune_registry_t* registry = tune_shared();
    (void)tune_set(registry, "render_lab.small_fill_ceiling", fill.ceiling);
    (void)tune_set(registry, "render_lab.small_fill_gate", fill.gate);
    (void)tune_set(registry, "render_lab.small_fill_style", fill.style);
#else
    (void)index;
#endif
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
}

int
render_lab_budget_ms(void) {
    return budget;
}
