/* Scene: load by name, find an entity and activate the first camera. */
#include "scene/scene.h"

static scene_t* hall;
static scene_entity_t statue;

static void
hall_example_start(void) {
    scene_failure_t why;
    hall = scene_load("hall", &why); /* NULL when an entry is missing: `why` names it */
    if (hall != NULL) {
        statue = scene_find(hall, name_hash("statue")); /* SCENE_ENTITY_NONE when there is none */
        scene_activate(hall, SCENE_CAMERA_FIRST);       /* its first camera */
    }
}
