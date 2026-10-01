/*
 * r3d_scene: a scenario's objects as the offline importer bakes them from a
 * scene file: mesh renderers with transforms, directional lights, and a
 * camera with the path it flies. The table is const data a scene reads
 * instead of hard-coding what to draw and where.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "anim/anim_track.h"
#include "render/camera.h"
#include "render/r3d_lit_mesh.h"
#include "render/vec3f.h"

/* Euler angles [pitch, yaw, roll] in degrees, right-handed: roll about z,
 * then pitch about x, then yaw about y. A positive scale on every axis. */
typedef struct {
    vec3f_t position, rotation, scale;
} r3d_transform_t;

typedef struct {
    const char* name;
    r3d_transform_t transform;
    const r3d_lit_mesh_t* mesh;
} r3d_scene_renderer_t;

/* A directional light: the direction toward it is the transform's +Y axis. */
typedef struct {
    const char* name;
    r3d_transform_t transform;
    vec3f_t color;
    float intensity, disc_degrees;
} r3d_scene_light_t;

/* A glTF camera animation's two tracks and the clip that plays them. */
typedef struct {
    const anim_clip_t* clip;
    const anim_track_t* translation;
    const anim_track_t* rotation;
} r3d_scene_path_t;

typedef struct {
    const char* name;
    r3d_transform_t transform; /* the pose when there is no path */
    float half_fov_short_tan, near_z;
    bool has_region;
    vec3f_t region_lo, region_hi; /* the box the camera moves within */
    const r3d_scene_path_t* path; /* NULL for a camera that stays put */
} r3d_scene_camera_t;

typedef struct {
    const r3d_scene_renderer_t* renderers;
    int renderer_count;
    const r3d_scene_light_t* lights;
    int light_count;
    const r3d_scene_camera_t* camera; /* NULL when the scene has none */
} r3d_scene_t;

bool r3d_transform_is_identity(const r3d_transform_t* transform);

/* The renderer with this object name, or NULL. */
const r3d_scene_renderer_t* r3d_scene_find_renderer(const r3d_scene_t* scene, const char* name);

/* The loop's length, or 0 for a camera that stays put. */
uint32_t r3d_scene_camera_period_ms(const r3d_scene_camera_t* camera);

/* The eye and look direction t_ms into the path, which wraps at its period. */
void r3d_scene_camera_sample(const r3d_scene_camera_t* camera, uint32_t t_ms, vec3f_t* eye, vec3f_t* forward);

/* The camera t_ms into its path. */
camera_t r3d_scene_camera_at(const r3d_scene_camera_t* camera, uint32_t t_ms);
