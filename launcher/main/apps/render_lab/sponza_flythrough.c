#include "sponza_flythrough.h"

static const r3d_waypoint_t waypoints[] = {
    {{1100.0F, 160.0F, -35.0F}, {0.0F, 220.0F, -35.0F}},    {{500.0F, 160.0F, -60.0F}, {-300.0F, 260.0F, 120.0F}},
    {{-100.0F, 180.0F, 40.0F}, {-800.0F, 300.0F, -120.0F}}, {{-650.0F, 260.0F, -30.0F}, {-350.0F, 700.0F, -35.0F}},
    {{-550.0F, 560.0F, -40.0F}, {400.0F, 500.0F, 100.0F}},  {{0.0F, 720.0F, -80.0F}, {700.0F, 450.0F, -35.0F}},
    {{500.0F, 450.0F, 0.0F}, {-300.0F, 150.0F, -35.0F}},    {{700.0F, 170.0F, -35.0F}, {-100.0F, 200.0F, -35.0F}},
};

const r3d_path_t sponza_flythrough = {
    waypoints,
    (int)(sizeof(waypoints) / sizeof(waypoints[0])),
    110.0F, /* cm per second, a slow walk */
    3.0F,
};

int
sponza_poses(r3d_vec3f_t* eye, r3d_vec3f_t* forward, int max) {
    const uint32_t period = r3d_path_period_ms(&sponza_flythrough);
    int count = 0;
    for (uint32_t t_ms = 0; t_ms < period && count < max; t_ms += SPONZA_POSE_EVERY_MS, count++) {
        r3d_path_sample(&sponza_flythrough, t_ms, &eye[count], &forward[count]);
    }
    return count;
}

void
sponza_view_at(r3d_lit_view_t* view, uint32_t t_ms, int position_scale, int quarter) {
    r3d_vec3f_t eye;
    r3d_vec3f_t forward;
    r3d_path_sample(&sponza_flythrough, t_ms, &eye, &forward);
    r3d_lit_view_look(view, eye, forward, SPONZA_HALF_FOV_SHORT_TAN, SPONZA_NEAR_Z, position_scale,
                      (r3d_viewport_t){SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT, quarter});
}
