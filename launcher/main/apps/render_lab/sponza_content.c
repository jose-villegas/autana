#include "sponza_content.h"

const char* const sponza_bakes[SPONZA_BAKE_COUNT] = {
    [SPONZA_BAKE_FULL] = "atrium",
    [SPONZA_BAKE_FLAT] = "atrium_flat",
    [SPONZA_BAKE_LITE] = "atrium_lite",
    [SPONZA_BAKE_FITTED] = "atrium_fitted",
    [SPONZA_BAKE_FITTED_FULL] = "atrium_fitted_full",
};

const resolution_step_t sponza_ladder[SPONZA_LADDER_STEPS] = {
    {368, 448}, {368, 358}, {368, 298}, {368, 224}, {245, 224}, {184, 224}, {147, 179}, {92, 112},
};

const resolution_model_t sponza_ladder_model = {
    7395.0F, 1.3170F, 3.4912F, 46995.0F, {13067, 11495, 10560, 10350, 9421, 5854, 8562, 5544}};
