/* Transform a point into a camera's view and interpolate a rotation. */
#include "math/linear/transformf.h"

typedef struct {
    vec3f_t in_camera;
    quatf_t halfway;
} transform_example_t;

void
transform_example(transform_example_t* s) {
    transformf_t body = TRANSFORMF_IDENTITY;
    transformf_set_position(&body, (vec3f_t){1.0F, 0.0F, 4.0F});
    transformf_set_rotation(&body, quatf_from_euler((vec3f_t){0.0F, MATH_PI / 2.0F, 0.0F}));

    const mat4f_t model = transformf_matrix(&body); /* built now, kept */
    const vec3f_t world = mat4f_apply(&model, (vec3f_t){0.0F, 0.0F, 1.0F});

    transformf_t camera = TRANSFORMF_IDENTITY;
    transformf_look_at(&camera, world, (vec3f_t){0.0F, 1.0F, 0.0F});
    const mat4f_t view = transformf_view(&camera);
    const vec3f_t in_camera = mat4f_apply(&view, world); /* straight ahead: x = y = 0 */

    const quatf_t halfway = quatf_slerp(quatf_identity(), body.rotation, 0.5F);
    s->in_camera = in_camera;
    s->halfway = halfway;
}
