/* app_physics_lab: the development lab for physics components. */
#include "apps/physics_lab/physics_lab.h"
#include "apps/physics_lab/physics_presets.h"

#define PHYSICS_LAB_NAME "physics_lab"
#define PHYSICS_LAB_WORD "physics"

LAB_VIEW(physics_lab_view_tunables, PHYSICS_LAB_NAME);

static const lab_scene_t* const scenes[] = {&scene_physics_stage, PHYSICS_PRESETS(PHYSICS_PRESET_SCENE_ENTRY)};

lab_t physics_lab = {
    .app = &app_physics_lab,
    .console_word = PHYSICS_LAB_WORD,
    .view = &physics_lab_view_tunables,
    .scenes = scenes,
    .scene_count = (int)(sizeof scenes / sizeof scenes[0]),
    .show_hud = true,
};

LAB_REGISTER(physics_lab);

bool
physics_lab_console(const char* args) {
    return lab_console(&physics_lab, args);
}

APP_CONSOLE(PHYSICS_LAB_WORD, physics_lab_console);

app_t app_physics_lab = {
    .name = "Physics Lab",
    .kind = APP_KIND_LAB,
    .summary = "Physics components",
    LAB_APP_CALLBACKS(physics_lab),
    .home_gesture = true,
    .console = APP_CONSOLE_PTR(physics_lab_console),
};

APP_REGISTER(app_physics_lab);
