/* r3d_scene: a camera lens and pose, and a placement made from a pose. */
#pragma once

#include <stdint.h>

#include "anim/anim_binding.h"
#include "anim/anim_track.h"
#include "math/linear/transformf.h"
#include "render/r3d_instance.h"
#include "render/render_view.h"

typedef struct {
    float half_fov_short_tan, near_z;
    const transformf_t* pose; /* entity TRS; NULL: the origin, looking down +z */
} r3d_scene_camera_t;

#define ANIM_COMPONENT_CAMERA ASSET_TYPE('C', 'A', 'M', 'R')
extern const anim_component_fields_t R3D_SCENE_CAMERA_FIELDS;

/* The pose's rotation times scale as m, its position as position. */
r3d_placement_t r3d_placement_from(const transformf_t* pose);

/* The eye and look direction at the camera pose. */
void r3d_scene_camera_sample(const r3d_scene_camera_t* camera, vec3f_t* eye, vec3f_t* forward);

/* The view through `camera`, framed for `viewport`. */
render_view_t r3d_scene_view_at(const r3d_scene_camera_t* camera, viewport_t viewport);
