#include "render/render_view.h"

static void
view_frame(render_view_t* view, vec3f_t right, vec3f_t down, float half_fov_short_tan, viewport_t viewport) {
    const viewport_quarter_axes_t axes = viewport_quarter_axes(viewport.quarter);
    const int shorter = viewport.width < viewport.height ? viewport.width : viewport.height;
    view->screen_x = vec3f_add(vec3f_scale(right, (float)axes.x_right), vec3f_scale(down, (float)axes.x_down));
    view->screen_y = vec3f_add(vec3f_scale(right, (float)axes.y_right), vec3f_scale(down, (float)axes.y_down));
    view->pixels_per_unit = (float)shorter / (2.0F * half_fov_short_tan);
    view->center_x = (float)viewport.width * 0.5F;
    view->center_y = (float)viewport.height * 0.5F;
    view->viewport = viewport;
}

render_view_t
render_view_make(const transformf_t* pose, float half_fov_short_tan, float near_z, viewport_t viewport) {
    /* Baked scenes use a right-handed frame, so picture right is pose -x. */
    const vec3f_t right = quatf_rotate(pose->rotation, (vec3f_t){-1.0F, 0.0F, 0.0F});
    const vec3f_t down = quatf_rotate(pose->rotation, (vec3f_t){0.0F, -1.0F, 0.0F});
    render_view_t view = {
        .position = pose->position,
        .forward = quatf_rotate(pose->rotation, (vec3f_t){0.0F, 0.0F, 1.0F}),
        .near_z = near_z,
    };
    view_frame(&view, right, down, half_fov_short_tan, viewport);
    return view;
}

void
render_view_refit(render_view_t* view, viewport_t viewport) {
    const viewport_quarter_axes_t before = viewport_quarter_axes(view->viewport.quarter);
    const vec3f_t right = vec3f_add(vec3f_scale(view->screen_x, (float)before.x_right),
                                    vec3f_scale(view->screen_y, (float)before.y_right));
    const vec3f_t down =
        vec3f_add(vec3f_scale(view->screen_x, (float)before.x_down), vec3f_scale(view->screen_y, (float)before.y_down));
    const int shorter = view->viewport.width < view->viewport.height ? view->viewport.width : view->viewport.height;
    const float half_fov_short_tan = (float)shorter / (2.0F * view->pixels_per_unit);
    view_frame(view, right, down, half_fov_short_tan, viewport);
    view->viewport = viewport;
}
