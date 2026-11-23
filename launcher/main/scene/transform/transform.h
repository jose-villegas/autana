/*
 * transform: each entity's position, rotation and scale in its parent's
 * frame. The world pose includes its ancestors and the scene root.
 * An applied constraint overwrites app writes to its owner's local pose.
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
