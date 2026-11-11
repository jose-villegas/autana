/*
 * render_settings: the quality and cost choices a render context draws with,
 * one value an app sets in one call (render_context_set_settings()) or
 * authors as a NAME.render.toml its pack names. A development override
 * (TUNE render.cull and the others) goes on top of it without writing into it.
 */
#pragma once

#include <stdbool.h>

#include "asset/asset_pack.h"

/* Each field, then its tier and what it trades: docs/render/Render-Settings.md
 * reads these lines. Basic is for any app; advanced needs the renderer's
 * workings to judge. */
typedef struct {
    float tolerance_px;  /* basic: the projected error a group may have and still draw coarse; more saves, pops more */
    float hysteresis_px; /* advanced: how far past the tolerance a coarse group's error goes before it turns fine */
    int coverage_permille; /* basic: the thousandths of the picture's height a coarse group may span; smaller pops less */
    int pick_every; /* advanced: the frames the pick is spread over, a scene's first frame picking all; more saves, switches later */
    int triangle_budget; /* basic: the triangles a frame aims to draw, the tolerance rising past tolerance_px to keep within; 0 never rises */
} render_lod_settings_t;

#define RENDER_ANIM_MID_PX        48
#define RENDER_ANIM_FAR_PX        20
#define RENDER_ANIM_MID_EVERY     2
#define RENDER_ANIM_FAR_EVERY     4
#define RENDER_ANIM_HYSTERESIS_PX 4
#define RENDER_ANIM_EVERY_MIN     1
#define RENDER_ANIM_EVERY_MAX     4
#define RENDER_ANIM_PIXELS_MAX    4096

typedef struct {
    float mid_px;        /* basic: radius below which poses run at mid_every */
    float far_px;        /* basic: radius below which poses run at far_every */
    int mid_every;       /* basic: frames between mid-sized poses; 1 poses every frame */
    int far_every;       /* basic: frames between far poses; 1 poses every frame */
    float hysteresis_px; /* advanced: pixels past a tier boundary before switching */
} render_anim_settings_t;

typedef struct {
    bool cull;                   /* basic: skips clusters and world boxes outside the picture */
    bool depth_fit;              /* advanced: depth steps go where the kept scene is, not to near_z; needs cull */
    bool batched;                /* advanced: one transform and one draw fork/join a batch, not an instance */
    bool partial_redraw;         /* basic: a still camera redraws only the rows that moved; the picture is the same */
    render_lod_settings_t lod;   /* which level of a mesh's clusters an instance draws (docs/plans/Cluster-LOD.md) */
    render_anim_settings_t anim; /* how often skeleton poses follow projected radius */
} render_settings_t;

/* RENDER_SETTINGS_DEFAULT, from default.render.toml beside this header. */
#include "render/settings/render_settings_default.h"

/* The pack entry a .pack.toml's `render` key bakes: a TABL with one row per field,
 * a float in 1/2^RENDER_SETTINGS_FRAC_BITS steps, a bool or an int as itself. */
#define RENDER_SETTINGS_ENTRY     "render"
#define RENDER_SETTINGS_FRAC_BITS 4

/* Reads `pack`'s RENDER_SETTINGS_ENTRY: a row for every field and no other.
 * ASSET_ERR_NOT_FOUND when the pack has none; `out` is written only on ASSET_OK. */
asset_status_t render_settings_read(const asset_pack_t* pack, render_settings_t* out);
