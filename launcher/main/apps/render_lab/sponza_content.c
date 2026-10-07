#include "sponza_content.h"

const char* const sponza_bakes[SPONZA_BAKE_COUNT] = {
    [SPONZA_BAKE_FULL] = "atrium",
    [SPONZA_BAKE_FLAT] = "atrium_flat",
    [SPONZA_BAKE_LITE] = "atrium_lite",
    [SPONZA_BAKE_FITTED] = "atrium_fitted",
    [SPONZA_BAKE_FITTED_FULL] = "atrium_fitted_full",
};

const resolution_step_t sponza_ladder[SPONZA_LADDER_STEPS] = {
    {368, 448}, {368, 358}, {368, 298}, {368, 224}, {184, 224}, {184, 179}, {184, 149},
};

const resolution_model_t sponza_ladder_model = {
    6080.0F, 1.6339F, 3.0254F, 46831.0F, {10368, 9371, 8233, 7260, 5861, 5602, 5419}};
