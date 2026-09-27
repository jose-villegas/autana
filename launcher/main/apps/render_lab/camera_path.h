/*
 * camera_path - a closed Catmull-Rom loop through camera waypoints, each an
 * eye and the point it looks at. A segment's duration follows the distance
 * its eye travels, so the camera moves at a steady speed, with a floor so a
 * segment that mostly turns still takes time.
 */
#pragma once

#include <stdint.h>

#include "lit_pipeline.h"

typedef struct {
    lit_vec3_t eye, target;
} camera_waypoint_t;

typedef struct {
    const camera_waypoint_t* points;
    int count; /* at least 2 */
    float units_per_second;
    float min_segment_seconds;
} camera_path_t;

uint32_t camera_path_period_ms(const camera_path_t* path);

/* t_ms wraps at the period; `forward` is target minus eye, unnormalised. */
void camera_path_sample(const camera_path_t* path, uint32_t t_ms, lit_vec3_t* eye, lit_vec3_t* forward);
