/* camera_poses: reference pose files shared by host camera tools. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "math/linear/vec3f.h"

typedef struct {
    vec3f_t eye, forward;
} camera_pose_t;

typedef struct {
    int width, height, count;
    float lens, near_z;
    camera_pose_t* poses;
} camera_poses_t;

/* Reads source poses into the engine frame. A dash reads standard input. */
bool camera_poses_load(const char* path, camera_poses_t* poses, char* problem, size_t problem_size);
void camera_poses_free(camera_poses_t* poses);
