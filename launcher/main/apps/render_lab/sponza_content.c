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
    6909.0F, 1.4831F, 3.2274F, 48099.0F, {9938, 9050, 8140, 7202, 9447, 5899, 8518, 8375}};
