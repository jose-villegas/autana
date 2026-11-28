/*
 * render_counts: a host harness's --counts rows, one per frame of what the
 * cull view counted from the camera (raster_cull_frame_t), as CSV under a
 * header of their names. The frame must be drawn with the cull view. Also
 * the renderer's path hits (--path-hits) as CSV and scene poses as JSON.
 */
#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "render_host.h"

/* render_rows_open() (render_host.h) under the counts' header. */
FILE* render_counts_open(const char* path, const char* tool);

/* The frame just drawn, as one row; after the last frame the file is
 * closed and `*rows` cleared. */
void render_counts_row(FILE** rows, const provider_frame_t* frame);

/* JSON array of camera/lens and skinned-entity poses, using render_rows_open(path, "[", tool). */
void render_pose_row(FILE** rows, const provider_frame_t* frame);

/* Every renderer path (render/path/r3d_path.h) and its hits so far, a
 * "path,hits" row each under that header; false, saying why under `tool`'s
 * name, when `path` cannot be written. */
bool render_path_hits_write(const char* path, const char* tool);
