/* render_view_fixture: a render_view_t from an eye-and-forward camera, upright, for suites. */
#pragma once

#include "render/raster.h"
#include "render/render_view.h"

typedef struct {
    vec3f_t eye, forward;
    float half_fov_short_tan, near_z;
} fixture_camera_t;

static inline render_view_t
render_view_fixture_at(const fixture_camera_t* camera, viewport_t viewport) {
    const transformf_t pose = transformf_looking(camera->eye, camera->forward, (vec3f_t){0.0F, 1.0F, 0.0F});
    return render_view_make(&pose, camera->half_fov_short_tan, camera->near_z, viewport);
}

static inline render_view_t
render_view_fixture(const fixture_camera_t* camera, const raster_t* raster, int quarter) {
    return render_view_fixture_at(camera, raster_viewport(raster, quarter));
}
