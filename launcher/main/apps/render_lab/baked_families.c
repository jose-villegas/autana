#include "baked_families.h"

#include "sponza_content.h"

static const char* const bistro_bakes[SCENE_BAKED_VARIANT_COUNT] = {
    [SCENE_BAKED_FULL] = "street",
    [SCENE_BAKED_FLAT] = "street_flat",
    [SCENE_BAKED_LITE] = "street_lite",
    [SCENE_BAKED_FITTED] = "street_fitted",
    [SCENE_BAKED_FITTED_FULL] = "street_fitted_full",
    [SCENE_BAKED_FLAT_FITTED] = "street_flat_fitted",
};

const scene_baked_t baked_sponza = {SPONZA_SCENE, sponza_bakes, SPONZA_CAMERA, false};
const scene_baked_t baked_bistro = {BISTRO_SCENE, bistro_bakes, BISTRO_CAMERA, true};

const scene_baked_t* const baked_families[BAKED_FAMILY_COUNT] = {&baked_sponza, &baked_bistro};
