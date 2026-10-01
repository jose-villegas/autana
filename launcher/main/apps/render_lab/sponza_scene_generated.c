/*
 * GENERATED FILE - do not edit.
 *
 *     python launcher/tools/r3d/mesh_import.py launcher/main/apps/render_lab/meshes/sponza.scene.toml
 */

#include "sponza_scene_generated.h"

#include "sponza_flat_mesh_generated.h"
#include "sponza_lite_mesh_generated.h"
#include "sponza_mesh_generated.h"
#include "flythrough_tracks_generated.h"

static const r3d_scene_renderer_t sponza_scene_renderers[] = {
    {"atrium", {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}, &sponza_mesh},
    {"atrium_flat", {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}, &sponza_flat_mesh},
    {"atrium_lite", {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}, &sponza_lite_mesh},
};

static const r3d_scene_light_t sponza_scene_lights[] = {
    {"sun", {{0.0F, 0.0F, 0.0F}, {18.4186102F, -48.6522228F, 0.0F}, {1.0F, 1.0F, 1.0F}}, {1.0F, 0.92F, 0.78F}, 3.0F, 1.2F},
};

static const r3d_scene_path_t sponza_scene_camera_path = {&flythrough_clip, &flythrough_camera_translation, &flythrough_camera_rotation};

static const r3d_scene_camera_t sponza_scene_camera = {
    "camera", {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}, 0.62F, 6.0F,
    true, {-1400.0F, 20.0F, -620.0F}, {1270.0F, 1250.0F, 550.0F}, &sponza_scene_camera_path,
};

const r3d_scene_t sponza_scene = {
    sponza_scene_renderers, 3,
    sponza_scene_lights, 1,
    &sponza_scene_camera,
};
