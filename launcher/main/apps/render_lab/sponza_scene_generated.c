/*
 * GENERATED FILE - do not edit.
 *
 *     python launcher/tools/r3d/scene_table.py launcher/main/apps/render_lab/meshes/sponza.scene.toml
 */

#include <stddef.h>

#include "sponza_scene_generated.h"

#include "sponza_flat_mesh_generated.h"
#include "sponza_lite_mesh_generated.h"
#include "sponza_mesh_generated.h"
#include "flythrough_tracks_generated.h"

const r3d_instance_t sponza_scene_atrium = {.mesh = &sponza_mesh, .placement = NULL};

const r3d_instance_t sponza_scene_atrium_flat = {.mesh = &sponza_flat_mesh, .placement = NULL};

const r3d_instance_t sponza_scene_atrium_lite = {.mesh = &sponza_lite_mesh, .placement = NULL};

static const r3d_scene_path_t sponza_scene_camera_path = {.clip = &flythrough_clip, .translation = &flythrough_camera_translation, .rotation = &flythrough_camera_rotation};

const r3d_scene_camera_t sponza_scene_camera = {.half_fov_short_tan = 0.62F, .near_z = 6.0F, .placement = NULL, .path = &sponza_scene_camera_path};
