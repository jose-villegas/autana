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
    6730.0F, 1.4723F, 3.2152F, 46424.0F, {10361, 9414, 8300, 7337, 8738, 5897, 7756, 7586}};
