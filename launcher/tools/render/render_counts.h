/*
 * render_counts: a host harness's --counts rows, one per frame of what the
 * cull view counted from the camera (raster_cull_frame_t), as CSV under a
 * header of their names. The frame must be drawn with the cull view.
 */
#pragma once

#include <stdio.h>

#include "render_host.h"

/* Opens `path` and writes the header; NULL, saying why under `tool`'s name, when it cannot. */
FILE* render_counts_open(const char* path, const char* tool);

/* The frame just drawn, as one row. */
void render_counts_row(FILE* rows, const provider_frame_t* frame);
