/*
 * r3d_scene: a baked camera: its lens, where it stands and the animation
 * it flies, sampled at a time; and a placement made from a pose. Reads
 * anim/ tracks; the raster does not.
 */
#pragma once

#include <stdint.h>

#include "anim/anim_binding.h"
#include "anim/anim_track.h"
#include "math/linear/transformf.h"
#include "render/r3d_instance.h"
#include "render/render_view.h"

/* A camera path's two tracks and the clip that plays them, by
 * value; the tracks point at their keys, which are not copied. */
typedef struct {
    anim_clip_t clip;
    anim_track_t translation;
    anim_track_t rotation;
} r3d_scene_path_t;

typedef struct {
    float half_fov_short_tan, near_z;
    const r3d_placement_t* placement; /* where it stands without a path; NULL: the origin, looking down +z */
    const r3d_scene_path_t* path;     /* NULL for a camera that stays put */
} r3d_scene_camera_t;

#define ANIM_COMPONENT_CAMERA ASSET_TYPE('C', 'A', 'M', 'R')
extern const anim_component_fields_t R3D_SCENE_CAMERA_FIELDS;

/* The pose's rotation times scale as m, its position as position. */
r3d_placement_t r3d_placement_from(const transformf_t* pose);

/* The loop's length, or 0 for a camera that stays put. */
uint32_t r3d_scene_camera_period_ms(const r3d_scene_camera_t* camera);

/* The eye and look direction t_ms into the path, which wraps at its period. */
void r3d_scene_camera_sample(const r3d_scene_camera_t* camera, uint32_t t_ms, vec3f_t* eye, vec3f_t* forward);

/* The view through `camera` t_ms into its path, framed for `viewport`. */
render_view_t r3d_scene_view_at(const r3d_scene_camera_t* camera, uint32_t t_ms, viewport_t viewport);
