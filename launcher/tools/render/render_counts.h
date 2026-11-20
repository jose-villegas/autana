/* render_counts: per-frame host cull counts as CSV and scene poses as JSON. */
#pragma once

#include <stdio.h>

#include "render_host.h"

/* Opens `path` and writes `header` as its first line; NULL, saying why
 * under `tool`'s name, when it cannot. Any per-frame CSV a harness writes. */
FILE* render_rows_open(const char* path, const char* header, const char* tool);

/* render_rows_open() under the counts' header. */
FILE* render_counts_open(const char* path, const char* tool);

/* The frame just drawn, as one row; after the last frame the file is
 * closed and `*rows` cleared. */
void render_counts_row(FILE** rows, const provider_frame_t* frame);

/* JSON array of camera/lens and skinned-entity poses, using render_rows_open(path, "[", tool). */
void render_pose_row(FILE** rows, const provider_frame_t* frame);
