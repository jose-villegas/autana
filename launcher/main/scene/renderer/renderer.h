/* renderer: the mesh an entity draws, static or skinned. */
#pragma once

#include <stdbool.h>

#include "render/r3d_instance.h"
#include "scene/scene.h"

/* The mesh the entity draws, and its pack id; NULL when it draws none. */
const r3d_lit_mesh_t* renderer_mesh(entity_t entity);
const char* renderer_mesh_id(entity_t entity);

/* The renderer draws `mesh` instead of the one it loaded: a mesh made at run
 * time, which the caller keeps alive for the scene's life. The renderer keeps
 * a copy of the view, so call again after each rebuild changes its counts. A
 * mesh with no nodes draws nothing. False when the entity has no renderer or
 * a skinned one, whose mesh its animator writes, or when `mesh` has more
 * levels of detail than the coarse bits the scene opened for the loaded mesh
 * hold. */
bool renderer_set_mesh(entity_t entity, const r3d_lit_mesh_t* mesh);

/* Skins the entity's renderer with the pack's skin `skin_id`, posed by its
 * skeleton animator (animator_set_clips()). False, and `why`, which may be
 * NULL, says why, when it cannot. */
bool renderer_set_skin(entity_t entity, const char* skin_id, scene_failure_t* why);
