#include "render/render_view.h"

#include <assert.h>

static void
view_frame(render_view_t* view, vec3f_t right, vec3f_t down, float half_fov_short_tan, viewport_t viewport) {
    const viewport_quarter_axes_t axes = viewport_quarter_axes(viewport.quarter);
    const int shorter = viewport.width < viewport.height ? viewport.width : viewport.height;
    view->screen_x = vec3f_add(vec3f_scale(right, (float)axes.x_right), vec3f_scale(down, (float)axes.x_down));
    view->screen_y = vec3f_add(vec3f_scale(right, (float)axes.y_right), vec3f_scale(down, (float)axes.y_down));
    view->lens.fx = (float)shorter / (2.0F * half_fov_short_tan);
    view->lens.fy = view->lens.fx;
    view->lens.cx = (float)viewport.width * 0.5F;
    view->lens.cy = (float)viewport.height * 0.5F;
    view->viewport = viewport;
}

render_view_t
render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport) {
    const vec3f_t right = quatf_rotate(pose->rotation, (vec3f_t){1.0F, 0.0F, 0.0F});
    const vec3f_t down = quatf_rotate(pose->rotation, (vec3f_t){0.0F, -1.0F, 0.0F});
    render_view_t view = {
        .position = pose->position,
        .forward = quatf_rotate(pose->rotation, (vec3f_t){0.0F, 0.0F, 1.0F}),
        .lens.near_z = near_z,
        .small_fill = SMALL_FILL_SMOOTH,
    };
    view_frame(&view, right, down, half_fov_short_tan, viewport);
    return view;
}

void
render_view_refit(render_view_t* view, viewport_t viewport) {
    assert(view->lens.fx == view->lens.fy);
    const viewport_quarter_axes_t before = viewport_quarter_axes(view->viewport.quarter);
    const vec3f_t right = vec3f_add(vec3f_scale(view->screen_x, (float)before.x_right),
                                    vec3f_scale(view->screen_y, (float)before.y_right));
    const vec3f_t down =
        vec3f_add(vec3f_scale(view->screen_x, (float)before.x_down), vec3f_scale(view->screen_y, (float)before.y_down));
    view_frame(view, right, down, render_view_half_fov_short_tan(view), viewport);
}

float
render_view_half_fov_short_tan(const render_view_t* view) {
    const int shorter = view->viewport.width < view->viewport.height ? view->viewport.width : view->viewport.height;
    return (float)shorter / (2.0F * view->lens.fx);
}
