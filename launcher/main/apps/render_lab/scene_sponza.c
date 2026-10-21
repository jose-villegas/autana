/*
 * scene_sponza: Crytek Sponza flown through on a looping camera path,
 * one scene per bake (sponza_bakes), shown through scene_baked.
 */

#include "render_lab_scene.h"
#include "scene_baked.h"
#include "sponza_content.h"

static const scene_baked_t sponza = {SPONZA_SCENE, sponza_bakes, SPONZA_BAKE_COUNT, sponza_cameras};

SCENE_BAKED(scene_sponza, sponza, SPONZA_BAKE_FULL, "Sponza", "Sponza", "sponza", "Full");
SCENE_BAKED(scene_sponza_lite, sponza, SPONZA_BAKE_LITE, "Sponza", "Sponza Lite", "sponza-lite", "Lite");
SCENE_BAKED(scene_sponza_flat, sponza, SPONZA_BAKE_FLAT, "Sponza", "Sponza Flat", "sponza-flat", "Flat");
SCENE_BAKED(scene_sponza_fitted, sponza, SPONZA_BAKE_FITTED, "Sponza", "Sponza Fitted", "sponza-fitted", "Fitted");
SCENE_BAKED(scene_sponza_fitted_full, sponza, SPONZA_BAKE_FITTED_FULL, "Sponza", "Sponza Fitted Full",
            "sponza-fitted-full", "Fitted Full");
SCENE_BAKED(scene_sponza_flat_fitted, sponza, SPONZA_BAKE_FLAT_FITTED, "Sponza", "Sponza Flat Fitted",
            "sponza-flat-fitted", "Flat Fitted");
