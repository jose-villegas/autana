/*
 * GENERATED FILE - do not edit.
 *
 *     python launcher/tools/r3d/scene_table.py launcher/main/apps/render_lab/meshes/sponza.scene.toml
 */

#include "sponza_scene_generated.h"

#include "sponza_flat_mesh_generated.h"
#include "sponza_lite_mesh_generated.h"
#include "sponza_mesh_generated.h"
#include "flythrough_tracks_generated.h"

const r3d_instance_t sponza_scene_atrium = {&sponza_mesh, NULL};

const r3d_instance_t sponza_scene_atrium_flat = {&sponza_flat_mesh, NULL};

const r3d_instance_t sponza_scene_atrium_lite = {&sponza_lite_mesh, NULL};

static const r3d_scene_path_t sponza_scene_camera_path = {&flythrough_clip, &flythrough_camera_translation, &flythrough_camera_rotation};

const r3d_scene_camera_t sponza_scene_camera = {0.62F, 6.0F, NULL, &sponza_scene_camera_path};
