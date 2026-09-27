#include "sponza_flythrough.h"

#define HALF_FOV_SHORT_TAN 0.62f
#define NEAR_Z             6.0f

static const r3d_waypoint_t waypoints[] = {
    {{1100.0f, 160.0f, -35.0f}, {0.0f, 220.0f, -35.0f}},    {{500.0f, 160.0f, -60.0f}, {-300.0f, 260.0f, 120.0f}},
    {{-100.0f, 180.0f, 40.0f}, {-800.0f, 300.0f, -120.0f}}, {{-650.0f, 260.0f, -30.0f}, {-350.0f, 700.0f, -35.0f}},
    {{-550.0f, 560.0f, -40.0f}, {400.0f, 500.0f, 100.0f}},  {{0.0f, 720.0f, -80.0f}, {700.0f, 450.0f, -35.0f}},
    {{500.0f, 450.0f, 0.0f}, {-300.0f, 150.0f, -35.0f}},    {{700.0f, 170.0f, -35.0f}, {-100.0f, 200.0f, -35.0f}},
};

const r3d_path_t sponza_flythrough = {
    waypoints,
    (int)(sizeof(waypoints) / sizeof(waypoints[0])),
    110.0f, /* cm per second, a slow walk */
    3.0f,
};

void
sponza_view_at(r3d_lit_view_t* view, uint32_t t_ms, int position_scale, int quarter) {
    r3d_vec3f_t eye, forward;
    r3d_path_sample(&sponza_flythrough, t_ms, &eye, &forward);
    r3d_lit_view_look(view, eye, forward, HALF_FOV_SHORT_TAN, NEAR_Z, position_scale,
                      (r3d_viewport_t){SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT, quarter});
}
