/*
 * scene_baked: a scene file drawn by its one fitted renderer.
 *
 * The shell advances the camera and draws it. A family is data
 * (baked_families.h): its scene, its renderer's name and its camera. A bake
 * whose pack mesh has textures gets a TEXTURES row for render_lab.textures
 * (render_lab_view.h).
 */
#pragma once

#include <stddef.h>

#include "baked_families.h"
#include "lab/lab_scene.h"

/* `family`'s bake under `root`; ENTITY_NONE, with `failure` naming it, when
 * the scene lacks it. */
entity_t scene_baked_find(entity_t root, const scene_baked_t* family, char* failure, size_t size);

/* The families render_lab shows. */
extern const lab_scene_t scene_sponza;
extern const lab_scene_t scene_bistro;
