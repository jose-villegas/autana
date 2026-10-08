#include "sponza_content.h"

const char* const sponza_bakes[SPONZA_BAKE_COUNT] = {
    [SPONZA_BAKE_FULL] = "atrium",
    [SPONZA_BAKE_FLAT] = "atrium_flat",
    [SPONZA_BAKE_LITE] = "atrium_lite",
    [SPONZA_BAKE_FITTED] = "atrium_fitted",
    [SPONZA_BAKE_FITTED_FULL] = "atrium_fitted_full",
    [SPONZA_BAKE_FLAT_FITTED] = "atrium_flat_fitted",
};

const resolution_step_t sponza_ladder[SPONZA_LADDER_STEPS] = {
    {368, 448}, {368, 358}, {368, 298}, {368, 224}, {184, 224}, {184, 179}, {184, 149},
};

const resolution_model_t sponza_ladder_model = {
    7852.0F, 1.4724F, 2.7419F, 52171.0F, {10376, 9365, 8252, 7304, 2752, 5575, 5428}};
