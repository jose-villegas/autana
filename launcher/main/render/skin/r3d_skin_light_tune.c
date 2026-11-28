/* The development overrides of the frame's skin lights: skin.albedo and skin.direct. */
#include "core/tune.h"
#include "render/skin/r3d_skin_light.h"

TUNE_OWNER(skin);
TUNE(skin, albedo, R3D_SKIN_TUNE_ALBEDO_SOURCE, R3D_SKIN_TUNE_ALBEDO_SOURCE, R3D_SKIN_TUNE_ALBEDO_WHITE);
TUNE(skin, direct, R3D_SKIN_TUNE_DIRECT_ON, R3D_SKIN_TUNE_DIRECT_OFF, R3D_SKIN_TUNE_DIRECT_ON);

uint32_t
r3d_skin_lights_tune(r3d_skin_lights_t* lights) {
    if (lights != NULL) {
        lights->white_albedo = albedo == R3D_SKIN_TUNE_ALBEDO_WHITE;
        if (direct == R3D_SKIN_TUNE_DIRECT_OFF) {
            lights->count = 0;
        }
    }
    return TUNE_GENERATION(skin);
}
