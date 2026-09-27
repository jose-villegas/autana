#include "sponza_flythrough.h"

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
