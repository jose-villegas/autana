/*
 * scene_bistro: the Bistro street walked and flown over on a looping camera path,
 * one scene per bake (bistro_bakes), shown through scene_baked.
 */

#include "bistro_content.h"
#include "render_lab_scene.h"
#include "scene_baked.h"

static const scene_baked_t bistro = {BISTRO_SCENE, bistro_bakes, BISTRO_BAKE_COUNT, bistro_cameras};

SCENE_BAKED(scene_bistro, bistro, BISTRO_BAKE_FULL, "Bistro", "Bistro", "bistro", "Full");
SCENE_BAKED(scene_bistro_lite, bistro, BISTRO_BAKE_LITE, "Bistro", "Bistro Lite", "bistro-lite", "Lite");
SCENE_BAKED(scene_bistro_flat, bistro, BISTRO_BAKE_FLAT, "Bistro", "Bistro Flat", "bistro-flat", "Flat");
SCENE_BAKED(scene_bistro_fitted, bistro, BISTRO_BAKE_FITTED, "Bistro", "Bistro Fitted", "bistro-fitted", "Fitted");
SCENE_BAKED(scene_bistro_fitted_full, bistro, BISTRO_BAKE_FITTED_FULL, "Bistro", "Bistro Fitted Full",
            "bistro-fitted-full", "Fitted Full");
SCENE_BAKED(scene_bistro_flat_fitted, bistro, BISTRO_BAKE_FLAT_FITTED, "Bistro", "Bistro Flat Fitted",
            "bistro-flat-fitted", "Flat Fitted");
