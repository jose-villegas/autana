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
    6954.0F, 1.4724F, 3.2395F, 47905.0F, {13084, 11463, 10635, 10398, 9435, 5851, 8549, 8390}};
