/* scene_conventions: what the scene bake and the runtime both assume, read
 * by the import settings. Kept apart so editing a layout never re-keys a bake. */
#pragma once

#include "math/linear/vec3f.h"

#define SCENE_LIGHT_AXIS ((vec3f_t){0.0F, 1.0F, 0.0F})
