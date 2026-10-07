#include "sponza_content.h"

const char* const sponza_bakes[SPONZA_BAKE_COUNT] = {
    [SPONZA_BAKE_FULL] = "atrium",
    [SPONZA_BAKE_FLAT] = "atrium_flat",
    [SPONZA_BAKE_LITE] = "atrium_lite",
    [SPONZA_BAKE_FITTED] = "atrium_fitted",
    [SPONZA_BAKE_FITTED_FULL] = "atrium_fitted_full",
};

const resolution_step_t sponza_ladder[SPONZA_LADDER_STEPS] = {
    {368, 448}, {368, 358}, {368, 298}, {368, 224}, {245, 224}, {184, 224}, {147, 179}, {122, 149},
};

const resolution_model_t sponza_ladder_model = {
    6747.0F, 1.4720F, 3.2125F, 46446.0F, {13376, 11788, 10909, 10703, 9797, 5885, 8933, 8788}};
