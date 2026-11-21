/* renderer: the mesh an entity draws, static or skinned. */
#pragma once

#include <stdbool.h>

#include "render/r3d_instance.h"
#include "scene/scene.h"

/* The mesh the entity draws, and its pack id; NULL when it draws none. */
const r3d_lit_mesh_t* renderer_mesh(entity_t entity);
const char* renderer_mesh_id(entity_t entity);

/* The renderer draws `mesh` instead of the one it loaded: a run-time mesh
 * the caller keeps alive for the scene's life. It keeps a copy of the view:
 * call again when a rebuild changes its counts. A mesh with no nodes draws
 * nothing. False when the entity has no renderer or a skinned one, whose
 * mesh its animator writes, when `mesh` has more levels of detail than the
 * coarse bits opened for the loaded mesh hold, or when a cut (MASK1), which
 * stays on, cannot take `mesh`. Textures `mesh` brings draw. */
bool renderer_set_mesh(entity_t entity, const r3d_lit_mesh_t* mesh);

/* Skins the entity's renderer with the pack's skin `skin_id`, posed by its
 * skeleton animator (animator_set_clips()). False, and `why`, which may be
 * NULL, says why, when it cannot. */
bool renderer_set_skin(entity_t entity, const char* skin_id, scene_failure_t* why);

/* Draws the entity's static renderer with its pack mesh's textures
 * (render/texture/) when `on`, else plain. They open once per mesh id, at
 * load when they cut, else at the first `on`, and stay open until the scene
 * unloads. False, and `why`, which may be NULL, says why, when the entity
 * has no static renderer, its mesh has no textures, they could not be
 * opened, they do not fit its mesh, or a cut would draw plain: off, or on
 * a mesh with face colours. */
bool renderer_set_textured(entity_t entity, bool on, scene_failure_t* why);

/* Whether the entity's static renderer's pack mesh has textures for
 * renderer_set_textured(), on or off; read from the pack, nothing opened. */
bool renderer_has_textures(entity_t entity);
