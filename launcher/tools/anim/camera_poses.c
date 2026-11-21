#include "camera_poses.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAMERA_POSES_LINE_BYTES 512
#define CAMERA_POSE_FIELDS      6

void
camera_poses_free(camera_poses_t* poses) {
    free(poses->poses);
    *poses = (camera_poses_t){0};
}

static bool
read_line(char* line, camera_poses_t* poses) {
    char* tag = line;
    while (isspace((unsigned char)*tag)) {
        tag++;
    }
    if (*tag == '\0') {
        return true;
    }
    char* values = tag;
    while (*values != '\0' && !isspace((unsigned char)*values)) {
        values++;
    }
    if (*values != '\0') {
        *values++ = '\0';
    }
    int end = 0;
    if (strcmp(tag, "size") == 0) {
        return sscanf(values, "%d %d %n", &poses->width, &poses->height, &end) == 2 && end > 0 && values[end] == '\0'
               && poses->width > 0 && poses->height > 0;
    }
    if (strcmp(tag, "lens") == 0) {
        return sscanf(values, "%f %f %n", &poses->lens, &poses->near_z, &end) == 2 && end > 0 && values[end] == '\0'
               && isfinite(poses->lens) && poses->lens > 0.0F && isfinite(poses->near_z) && poses->near_z > 0.0F;
    }
    camera_pose_t pose;
    if (strcmp(tag, "pose") != 0
        || sscanf(values, "%f %f %f %f %f %f %n", &pose.eye.x, &pose.eye.y, &pose.eye.z, &pose.forward.x,
                  &pose.forward.y, &pose.forward.z, &end)
               != CAMERA_POSE_FIELDS
        || end == 0 || values[end] != '\0' || !isfinite(pose.eye.x) || !isfinite(pose.eye.y) || !isfinite(pose.eye.z)
        || !isfinite(pose.forward.x) || !isfinite(pose.forward.y) || !isfinite(pose.forward.z)
        || (pose.forward.x == 0.0F && pose.forward.z == 0.0F) || poses->count == INT_MAX
        || (size_t)(poses->count + 1) > SIZE_MAX / sizeof(pose)) {
        return false;
    }
    camera_pose_t* grown = realloc(poses->poses, (size_t)(poses->count + 1) * sizeof(pose));
    if (grown == NULL) {
        return false;
    }
    poses->poses = grown;
    poses->poses[poses->count++] = pose;
    return true;
}

bool
camera_poses_load(const char* path, camera_poses_t* poses) {
    *poses = (camera_poses_t){0};
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    char line[CAMERA_POSES_LINE_BYTES];
    bool ok = true;
    while (ok && fgets(line, sizeof line, file) != NULL) {
        ok = (strchr(line, '\n') != NULL || feof(file)) && read_line(line, poses);
    }
    ok = ok && !ferror(file) && poses->width > 0 && poses->height > 0 && poses->lens > 0.0F && poses->near_z > 0.0F
         && poses->count > 0;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        camera_poses_free(poses);
    }
    return ok;
}
