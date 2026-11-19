/* light: a directional light on an entity, pointed by its world rotation. */
#pragma once

#include <stdbool.h>

#include "math/linear/vec3f.h"
#include "scene/entity/entity.h"

typedef enum { LIGHT_MODE_REALTIME, LIGHT_MODE_BAKED } light_mode_t;

/* Setters attach a realtime light, initially white with zero intensity; refusals change nothing. */
bool light_set_mode(entity_t entity, light_mode_t mode);
bool light_set_color(entity_t entity, vec3f_t linear_rgb);
bool light_set_intensity(entity_t entity, float intensity);
/* Realtime when the entity has no light. */
light_mode_t light_mode(entity_t entity);
/* Zero when the entity has no light. */
vec3f_t light_color(entity_t entity);
float light_intensity(entity_t entity);
