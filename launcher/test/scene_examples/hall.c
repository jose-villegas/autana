/* Scene: load by name, find an entity and activate the first camera. */
#include "scene/camera/camera.h"
#include "scene/scene.h"

static entity_t hall;
static entity_t statue;

static void
hall_example_start(void) {
    scene_failure_t why;
    hall = scene_load("hall");
    if (scene_state(hall) == PENDING_READY) {
        statue = entity_find(hall, name_hash("statue")); /* ENTITY_NONE when there is none */
        camera_activate(camera_first(hall));             /* its first camera */
    } else {
        (void)scene_failure(hall, &why); /* `why` names the missing entry */
        scene_release(hall);
        hall = ENTITY_NONE;
    }
}
