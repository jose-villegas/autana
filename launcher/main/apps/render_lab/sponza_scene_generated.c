/*
 * GENERATED FILE - do not edit.
 *
 *     python launcher/tools/r3d/scene_table.py launcher/main/apps/render_lab/meshes/sponza.scene.toml
 */

#include <stddef.h>

#include "sponza_scene_generated.h"

#include "flythrough_tracks_generated.h"

static r3d_lit_mesh_t sponza_scene_atrium_mesh;
const r3d_instance_t sponza_scene_atrium = {.mesh = &sponza_scene_atrium_mesh, .placement = NULL};

static r3d_lit_mesh_t sponza_scene_atrium_flat_mesh;
const r3d_instance_t sponza_scene_atrium_flat = {.mesh = &sponza_scene_atrium_flat_mesh, .placement = NULL};

static r3d_lit_mesh_t sponza_scene_atrium_lite_mesh;
const r3d_instance_t sponza_scene_atrium_lite = {.mesh = &sponza_scene_atrium_lite_mesh, .placement = NULL};

static const r3d_scene_mesh_t sponza_scene_meshes[] = {
    {"sponza", &sponza_scene_atrium_mesh},
    {"sponza_flat", &sponza_scene_atrium_flat_mesh},
    {"sponza_lite", &sponza_scene_atrium_lite_mesh},
};

const r3d_scene_assets_t sponza_scene_assets = {.meshes = sponza_scene_meshes, .count = (int)(sizeof sponza_scene_meshes / sizeof sponza_scene_meshes[0])};

static const r3d_scene_path_t sponza_scene_camera_path = {.clip = &flythrough_clip, .translation = &flythrough_camera_translation, .rotation = &flythrough_camera_rotation};

const r3d_scene_camera_t sponza_scene_camera = {.half_fov_short_tan = 0.62F, .near_z = 6.0F, .placement = NULL, .path = &sponza_scene_camera_path};
