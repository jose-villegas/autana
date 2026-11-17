/* light: a directional light on an entity, pointed by its world rotation. */
#pragma once

#include <stdbool.h>

#include "math/linear/vec3f.h"
#include "scene/entity/entity.h"

/* Setters attach a light, initially white with zero intensity; refusals change nothing. */
bool light_set_color(entity_t entity, vec3f_t linear_rgb);
bool light_set_intensity(entity_t entity, float intensity);
/* Zero when the entity has no light. */
vec3f_t light_color(entity_t entity);
float light_intensity(entity_t entity);
