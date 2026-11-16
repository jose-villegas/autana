/* load_scene_async: request on enter, activate its camera once READY, release on exit. */
#include "scene/camera/camera.h"
#include "scene/scene.h"

typedef struct {
    entity_t hall;
    bool shown;
} load_scene_example_t;

void
load_scene_enter(load_scene_example_t* visit, const char* id) {
    *visit = (load_scene_example_t){.hall = scene_load_async(id, NULL, HANDLE_NONE)};
}

void
load_scene_frame(load_scene_example_t* visit) {
    if (!visit->shown && scene_state(visit->hall) == PENDING_READY) {
        visit->shown = camera_activate(camera_first(visit->hall));
    }
}

void
load_scene_exit(load_scene_example_t* visit) {
    scene_release(visit->hall);
    *visit = (load_scene_example_t){0};
}
