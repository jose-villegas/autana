/*
 * triangle_sizes: how many of a lit mesh's drawn triangles cover 0, 1, 2-4
 * or more pixel centres from a lens, counted by the top-left rule from the
 * view itself, so any version of the pipeline means the same; and the poses
 * file the triangle_sizes tool reads.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "render/r3d_pipeline.h"
#include "render/vec3f.h"

enum { R3D_SIZES_ZERO, R3D_SIZES_ONE, R3D_SIZES_TWO_TO_FOUR, R3D_SIZES_MORE, R3D_SIZES_BINS };

typedef struct {
    long drawn;     /* wholly in front, facing, touching the screen */
    long box_empty; /* bounding box holds no pixel centre; also counted in bins[R3D_SIZES_ZERO] */
    long bins[R3D_SIZES_BINS];
    long box_two_by_two; /* bounding box holds at most 2 x 2 centres */
} r3d_sizes_t;

/* Adds the triangles of the `count` clusters in `visible` to `out`. */
void r3d_sizes_count(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* visible, int count,
                     r3d_sizes_t* out);

void r3d_sizes_add(r3d_sizes_t* total, const r3d_sizes_t* s);

#define R3D_SIZES_POSES_MAX 64

/* A poses file, one item a line, `#` starting a comment:
 *   size <width> <height>
 *   lens <half_fov_short_tan> <near_z>
 *   pose <eye x y z> <forward x y z>
 * in the mesh's model units, one pose line per view. */
typedef struct {
    int width, height;
    float half_fov_short_tan, near_z;
    int count;
    vec3f_t eye[R3D_SIZES_POSES_MAX];
    vec3f_t forward[R3D_SIZES_POSES_MAX];
} r3d_sizes_poses_t;

/* True when the file was read whole; else false, with what is wrong and on
 * which line written to `problem`. */
bool r3d_sizes_read_poses(FILE* f, r3d_sizes_poses_t* out, char* problem, size_t problem_size);

/* The same from a path, "-" reading standard input, so a generator can pipe
 * its poses in and no copy of them is kept to go stale. */
bool r3d_sizes_read_poses_path(const char* path, r3d_sizes_poses_t* out, char* problem, size_t problem_size);
