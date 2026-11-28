/* r3d_skin_light_tune: the development overrides of the frame's skin lights, skin.albedo and skin.direct. */
#pragma once

#include <stdint.h>

#include "render/skin/r3d_skin_light.h"

/* skin.albedo: SOURCE keeps each mesh's albedo, WHITE draws every skin white. skin.direct: OFF drops the direct
 * lights, ON keeps them. Defines, not an enum: a release build's tunables are enum constants of their own, and
 * comparing two enum types fails -Werror=enum-compare. */
#define R3D_SKIN_TUNE_ALBEDO_SOURCE 0
#define R3D_SKIN_TUNE_ALBEDO_WHITE  1
#define R3D_SKIN_TUNE_DIRECT_OFF    0
#define R3D_SKIN_TUNE_DIRECT_ON     1

/* `lights` (NULL for none) under skin.albedo and skin.direct. Returns the skin tunables' generation, which moves
 * when an override changes and is 0 in a release build. */
uint32_t r3d_skin_lights_tune(r3d_skin_lights_t* lights);
