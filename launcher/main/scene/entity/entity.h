/*
 * entity: the engine world's entities. One world holds every loaded scene;
 * each scene is a subtree under its root entity (scene_load()). An entity_t is
 * a handle: kept across frames, refused once its scene is released.
 */
#pragma once

#include <stdbool.h>

#include "core/handle.h"
#include "hash/name_hash.h"

typedef handle_t entity_t;
#define ENTITY_NONE HANDLE_NONE

/* The first entity below `within`, in load order, whose name hashes to
 * `name` (name_hash()), or ENTITY_NONE. A root is never found by name:
 * scene_load() returns it. Look it up once, when the scene is READY, and
 * keep it. */
entity_t entity_find(entity_t within, name_hash_t name);

/* The entity after `after` inside `within`, in load order, or ENTITY_NONE
 * when there is none; ENTITY_NONE starts. `within` itself is not visited. */
entity_t entity_next(entity_t within, entity_t after);

/* Its authored name: a root's is its scene id. NULL for a refused handle. */
const char* entity_name(entity_t entity);

/* A disabled entity is not drawn or animated, nor are its descendants;
 * every entity starts enabled. */
bool entity_enabled(entity_t entity);
void entity_set_enabled(entity_t entity, bool enabled);

/* Drawn and animated: enabled, and every ancestor enabled. */
bool entity_active(entity_t entity);

/* The entity's parent, or ENTITY_NONE for a scene root. */
entity_t entity_parent(entity_t entity);

/* True when `entity` is `ancestor` or one of its descendants. */
bool entity_is_within(entity_t entity, entity_t ancestor);
