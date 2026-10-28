#include "baked_families.h"

#include "sponza_content.h"

static const char* const bistro_bakes[DEMO_BAKED_VARIANT_COUNT] = {
    [DEMO_BAKED_FULL] = "street",
    [DEMO_BAKED_FLAT] = "street_flat",
    [DEMO_BAKED_LITE] = "street_lite",
    [DEMO_BAKED_FITTED] = "street_fitted",
    [DEMO_BAKED_FITTED_FULL] = "street_fitted_full",
    [DEMO_BAKED_FLAT_FITTED] = "street_flat_fitted",
};

const demo_baked_t baked_sponza = {SPONZA_SCENE, sponza_bakes, SPONZA_CAMERA};
const demo_baked_t baked_bistro = {BISTRO_SCENE, bistro_bakes, BISTRO_CAMERA};

const demo_baked_t* const baked_families[BAKED_FAMILY_COUNT] = {&baked_sponza, &baked_bistro};
