#include "sponza_content.h"

const char* const sponza_bakes[SCENE_BAKED_VARIANT_COUNT] = {
    [SCENE_BAKED_FULL] = "atrium",
    [SCENE_BAKED_FLAT] = "atrium_flat",
    [SCENE_BAKED_LITE] = "atrium_lite",
    [SCENE_BAKED_FITTED] = "atrium_fitted",
    [SCENE_BAKED_FITTED_FULL] = "atrium_fitted_full",
    [SCENE_BAKED_FLAT_FITTED] = "atrium_flat_fitted",
};

const char* const sponza_cameras[SPONZA_CAMERA_COUNT] = {SPONZA_CAMERA, "tour"};
