/*
 * r3d_path - a closed Catmull-Rom loop through camera waypoints, each an
 * eye and the point it looks at. A segment's duration follows the distance
 * its eye travels, so the camera moves at a steady speed, with a floor so a
 * segment that mostly turns still takes time.
 */
#pragma once

#include <stdint.h>

#include "render/r3d_lit_pipeline.h"

typedef struct {
    r3d_lit_vec3_t eye, target;
} r3d_waypoint_t;

typedef struct {
    const r3d_waypoint_t* points;
    int count; /* at least 2 */
    float units_per_second;
    float min_segment_seconds;
} r3d_path_t;

uint32_t r3d_path_period_ms(const r3d_path_t* path);

/* t_ms wraps at the period; `forward` is target minus eye, unnormalised. */
void r3d_path_sample(const r3d_path_t* path, uint32_t t_ms, r3d_lit_vec3_t* eye, r3d_lit_vec3_t* forward);
