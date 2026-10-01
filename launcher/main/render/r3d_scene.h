/*
 * r3d_scene: the camera of a baked table: its lens, where it stands and the
 * glTF animation it flies. The importer writes the table as const data, so
 * what to draw and from where is data. Reads anim/ tracks; the raster does not.
 */
#pragma once

#include <stdint.h>

#include "anim/anim_track.h"
#include "asset/asset_pack.h"
#include "render/camera.h"
#include "render/r3d_instance.h"

/* A glTF camera animation's two tracks and the clip that plays them. */
typedef struct {
    const anim_clip_t* clip;
    const anim_track_t* translation;
    const anim_track_t* rotation;
} r3d_scene_path_t;

typedef struct {
    float half_fov_short_tan, near_z;
    const r3d_placement_t* placement; /* where it stands without a path; NULL: the origin, looking down -Z */
    const r3d_scene_path_t* path;     /* NULL for a camera that stays put */
} r3d_scene_camera_t;

/* A mesh the scene draws by its asset id, and the view that names it in an
 * instance once r3d_scene_bind() has filled it. */
typedef struct {
    const char* asset;
    r3d_lit_mesh_t* mesh;
} r3d_scene_mesh_t;

typedef struct {
    const r3d_scene_mesh_t* meshes;
    int count;
} r3d_scene_assets_t;

/* Opens every mesh the scene names from `pack`. Stops at the first that is
 * missing or malformed: returns its status and points `failed` at its id,
 * the first mesh's id when `pack` is NULL. A NULL `failed` is allowed. */
asset_status_t r3d_scene_bind(const asset_pack_t* pack, const r3d_scene_assets_t* assets, const char** failed);

/* The loop's length, or 0 for a camera that stays put. */
uint32_t r3d_scene_camera_period_ms(const r3d_scene_camera_t* camera);

/* The eye and look direction t_ms into the path, which wraps at its period. */
void r3d_scene_camera_sample(const r3d_scene_camera_t* camera, uint32_t t_ms, vec3f_t* eye, vec3f_t* forward);

/* The camera t_ms into its path. */
camera_t r3d_scene_camera_at(const r3d_scene_camera_t* camera, uint32_t t_ms);
