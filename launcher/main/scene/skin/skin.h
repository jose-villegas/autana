/* skin: how a skinned renderer is lit. */
#pragma once

#include <stdbool.h>

#include "render/skin/r3d_skin_light.h"
#include "scene/entity/entity.h"

typedef enum {
    SKIN_LIGHT_DIRECT = R3D_SKIN_LIGHT_DIRECT,
    SKIN_LIGHT_RENORMALISED = R3D_SKIN_LIGHT_RENORMALISED,
    SKIN_LIGHT_TABLE = R3D_SKIN_LIGHT_TABLE
} skin_light_mode_t;

/* False for an entity without a lit skin, or a table its memory cannot hold. */
bool skin_set_light_mode(entity_t entity, skin_light_mode_t mode);
