/* Scene: load by name, find an entity and activate the first camera. */
#include "scene/scene.h"

static scene_t* hall;
static scene_entity_t statue;

static void
hall_example_start(const char* scene_id, const char* entity_name) {
    scene_failure_t why;
    hall = scene_load(scene_id, &why);
    if (hall != NULL) {
        statue = scene_find(hall, entity_name);
        scene_activate(hall, NULL);
    }
}
