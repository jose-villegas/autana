/* lens: a scene camera's lens; its pose is the camera entity's world transform. */
#pragma once

#include "anim/anim_binding.h"
#include "anim/anim_track.h"
#include "math/linear/transformf.h"
#include "render/render_view.h"

typedef struct {
    float half_fov_short_tan, near_z;
    camera_present_t present;
    small_fill_t small_fill;
    bool skip_hidden; /* render_view_t.skip_hidden */
    bool curves;      /* render_view_t.curves */
    vec2f_t shift;    /* render_view_t.shift */
} lens_t;

#define ANIM_COMPONENT_CAMERA ASSET_TYPE('C', 'A', 'M', 'R')
extern const anim_component_fields_t LENS_FIELDS;

/* The view through `lens` from `pose` (its rotation, roll included; scale ignored), framed for `viewport`. */
render_view_t lens_view(const lens_t* lens, const transformf_t* pose, viewport_t viewport);
