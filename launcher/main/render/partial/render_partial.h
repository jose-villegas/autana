/*
 * render_partial: while the camera holds still, a frame redraws only the
 * rows where something changed since the last one, and leaves the rest of
 * the raster and its upscaled picture as they were. The rows are those the
 * changed objects' world boxes cover through the view: each box holds where
 * an object was and where it is now.
 *
 * Anything else that moves the picture draws every row: a new view, size,
 * clear colour, settings or instance count, a debug view, a box behind the
 * eye, a depth scale the draw had to change, and a level pick (render/lod)
 * that changed a cut. Picks spread over frames are owed for a few frames
 * after a change, and those frames cull even when no row changed. On with
 * render_settings_t.partial_redraw.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/geometry/box3f.h"
#include "render/raster.h"
#include "render/render_view.h"
#include "render/settings/render_settings.h"

/* Changed boxes one frame holds; more draws every row. */
#define RENDER_PARTIAL_CHANGES_MAX 16

/* Rows a frame redraws: [first, end) of the raster, none when equal. */
typedef struct {
    int first, end;
} render_rows_t;

typedef struct {
    bool kept; /* the last frame left a whole picture to draw over */
    render_view_t view;
    int width, height, instance_count;
    uint16_t clear;
    upscale_filter_t filter;
    render_settings_t settings;
    box3f_t changed[RENDER_PARTIAL_CHANGES_MAX];
    int changed_count;           /* -1: more than fit, or the set itself changed */
    int still_frames;            /* frames since one with rows to redraw */
    const uint16_t* destination; /* where the last picture was composed */
    bool composed;               /* the last draw was composed there */
} render_partial_t;

/* This frame's changed boxes, world units, each where an object was and is
 * now; a negative count changes everything. */
void render_partial_set_changed(render_partial_t* partial, const box3f_t* boxes, int count);

/* The rows the next draw of `raster` through `view` redraws: every row
 * unless the kept picture can be drawn over. */
render_rows_t render_partial_rows(render_partial_t* partial, const raster_t* raster, const render_view_t* view);

/* Whether level picks spread over `pick_every` frames are owed: a frame
 * with rows to redraw came fewer than that many frames ago, so a still
 * instance may not yet have picked under this view. */
bool render_partial_picks_owed(const render_partial_t* partial, int pick_every);

/* The draw of `raster` through `view` left a whole picture. */
void render_partial_kept(render_partial_t* partial, const raster_t* raster, const render_view_t* view);

/* The picture is being composed into `destination`; false when its kept
 * rows are not there (a first or another destination), so every row is
 * composed. A draw left uncomposed draws every row next frame. */
bool render_partial_composed(render_partial_t* partial, const uint16_t* destination);

/* The picture is not to be drawn over: the next frame draws every row. */
void render_partial_forget(render_partial_t* partial);
