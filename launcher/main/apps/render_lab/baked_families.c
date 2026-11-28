#include "baked_families.h"

#include "sponza_content.h"

const scene_baked_t baked_sponza = {SPONZA_SCENE, "atrium_fitted_full", SPONZA_CAMERA};
const scene_baked_t baked_bistro = {BISTRO_SCENE, "street_fitted_full", BISTRO_CAMERA};

const scene_baked_t* const baked_families[BAKED_FAMILY_COUNT] = {&baked_sponza, &baked_bistro};
