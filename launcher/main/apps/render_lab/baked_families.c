#include "baked_families.h"

#include "sponza_content.h"

static const char* const bistro_bakes[SCENE_BAKED_VARIANT_COUNT] = SCENE_BAKED_NAMES("street");

const scene_baked_t baked_sponza = {SPONZA_SCENE, sponza_bakes, SPONZA_CAMERA};
const scene_baked_t baked_bistro = {BISTRO_SCENE, bistro_bakes, "camera"};

const scene_baked_t* const baked_families[BAKED_FAMILY_COUNT] = {&baked_sponza, &baked_bistro};
