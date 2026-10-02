/*
 * GENERATED FILE - do not edit.
 *
 *     python launcher/tools/r3d/scene_table.py launcher/main/apps/render_lab/meshes/sponza.scene.toml
 */

#include <stddef.h>

#include "sponza_scene_generated.h"

#include "scene/scene.h"

#include "flythrough_tracks_generated.h"

static const char* const sponza_scene_names[] = {"camera", "atrium", "atrium_flat", "atrium_lite", "atrium_fitted", "atrium_fitted_full"};

static const scene_transform_t sponza_scene_transforms[] = {
    {.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, .position = {0.0F, 0.0F, 0.0F}},
    {.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, .position = {0.0F, 0.0F, 0.0F}},
    {.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, .position = {0.0F, 0.0F, 0.0F}},
    {.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, .position = {0.0F, 0.0F, 0.0F}},
    {.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, .position = {0.0F, 0.0F, 0.0F}},
    {.m = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}, .position = {0.0F, 0.0F, 0.0F}},
};

static const scene_renderer_def_t sponza_scene_renderers[] = {
    {1, "sponza"},
    {2, "sponza_flat"},
    {3, "sponza_lite"},
    {4, "sponza_fitted"},
    {5, "sponza_fitted_full"},
};

static const r3d_scene_path_t sponza_scene_camera_path = {.clip = &flythrough_clip, .translation = &flythrough_camera_translation, .rotation = &flythrough_camera_rotation};

static const scene_camera_def_t sponza_scene_cameras[] = {
    {0, {.half_fov_short_tan = 0.62F, .near_z = 6.0F, .placement = NULL, .path = &sponza_scene_camera_path}, 0x9CC0E6},
};

const scene_def_t sponza_scene = {
    .name = "sponza",
    .entity_count = 6,
    .renderer_count = 5,
    .camera_count = 1,
    .entity_names = sponza_scene_names,
    .transforms = sponza_scene_transforms,
    .renderers = sponza_scene_renderers,
    .cameras = sponza_scene_cameras,
};

SCENE_REGISTER(sponza_scene)
