/*
 * Local TRS is in the parent's frame. World pose includes every ancestor.
 * Position constraints rebuild local position and aim constraints rebuild
 * local rotation. Hinges rebuild both and preserve local scale.
 */
#pragma once

#include "math/linear/mat4f.h"
#include "math/linear/transformf.h"
#include "scene/entity/entity.h"

/* The identity for a refused handle. */
transformf_t transform_local(entity_t entity);
void transform_set_local(entity_t entity, transformf_t local);

/* Where the entity stands in the world. Setting it stores the local that puts it there. */
transformf_t transform_world(entity_t entity);
void transform_set_world(entity_t entity, transformf_t world);

/* The same world as a matrix, for applying it to many points: the form the renderer draws with. */
mat4f_t transform_world_matrix(entity_t entity);
