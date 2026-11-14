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
/* lod.blend: how a group changes level (docs/render/frame/Levels-of-Detail.md#the-blend). */
typedef enum {
    RENDER_LOD_BLEND_SWITCH, /* at once */
    RENDER_LOD_BLEND_ON,     /* a mesh with morph targets moves onto the coarse surface first; any other draws both
                                levels through complementary masks */
    RENDER_LOD_BLEND_COUNT,
} render_lod_blend_t;

/* The longest lod.blend_px a pack may set: past it a group would blend
 * across most of the band where it stays coarse. */
#define RENDER_LOD_BLEND_PX_MAX 4.0F

typedef struct {
    float tolerance_px;  /* basic: the projected error a group may have and still draw coarse; more saves, pops more */
    float hysteresis_px; /* advanced: how far past the tolerance a coarse group's error goes before it turns fine */
    int coverage_permille; /* basic: the thousandths of the picture's height a coarse group may span; smaller pops less */
    int pick_every; /* advanced: the frames the pick is spread over, a scene's first frame picking all; more saves, switches later */
    int triangle_budget; /* basic: the triangles a frame aims to draw, the tolerance rising past tolerance_px to keep within; 0 never rises */
    int blend; /* basic: 0 switches at once; 1 blends each switch, by morph where a mesh was imported with geometry.morph, else by dither */
    float blend_px; /* advanced: the error past a switch over which a blend fades out; longer is softer, moves more */
} render_lod_settings_t;

#define RENDER_ANIM_EVERY_MIN  1
#define RENDER_ANIM_EVERY_MAX  4
#define RENDER_ANIM_PIXELS_MAX 4096

typedef struct {
    float mid_px;        /* basic: projected radius in pixels below which a copy is posed every mid_every frames */
    float far_px;        /* basic: projected radius in pixels below which a copy is posed every far_every frames */
    int mid_every;       /* basic: frames between mid-sized poses; 1 poses every frame */
    int far_every;       /* basic: frames between far poses; 1 poses every frame */
    float hysteresis_px; /* advanced: pixels past a tier boundary before switching */
} render_anim_settings_t;

/* The longest texture.divide_px a pack may set, and the longest a segment
 * grows to: an 8.8 step across it drifts under an eighth of a texel. */
#define RENDER_TEXTURE_DIVIDE_PX_MAX 32
/* The largest texture.affine_bend a pack may set, in texels. */
#define RENDER_TEXTURE_BEND_MAX      15.0F

typedef struct {
    int divide_px; /* basic: fewest pixels between divides by depth, fewer cost more; 0 is affine, near surfaces bend */
    float affine_bend; /* advanced: texels a triangle may bend and still step affine; larger is faster, bends more */
} render_texture_settings_t;

typedef struct {
    bool cull;                   /* basic: skips clusters and world boxes outside the picture */
    bool depth_fit;              /* advanced: depth steps go where the kept scene is, not to near_z; needs cull */
    bool batched;                /* advanced: one transform and one draw fork/join a batch, not an instance */
    bool partial_redraw;         /* basic: a still camera redraws only the rows that moved; the picture is the same */
    render_lod_settings_t lod;   /* the level pick (docs/render/frame/Levels-of-Detail.md) */
    render_anim_settings_t anim; /* pose rate by size (docs/render/animation/Skinned-Meshes.md) */
    render_texture_settings_t texture; /* the textured span fill (docs/assets/formats/LTEX.md) */
} render_settings_t;

/* RENDER_SETTINGS_DEFAULT, from default.render.toml beside this header. */
#include "render/settings/render_settings_default.h"

/* The pack entry a .pack.toml's `render` key bakes: a TABL with one row per field,
 * a float in 1/2^RENDER_SETTINGS_FRAC_BITS steps, a bool or an int as itself. */
#define RENDER_SETTINGS_ENTRY     "render"
#define RENDER_SETTINGS_FRAC_BITS 4

/* Reads `pack`'s RENDER_SETTINGS_ENTRY: a row for every field and no other,
 * lod.blend a render_lod_blend_t, lod.blend_px above 0 and at most
 * RENDER_LOD_BLEND_PX_MAX, the anim intervals RENDER_ANIM_EVERY_MIN to _MAX
 * and its pixels 0 to RENDER_ANIM_PIXELS_MAX (else ASSET_ERR_FORMAT). ASSET_ERR_NOT_FOUND when
 * the pack has none; `out` is written only on ASSET_OK. */
asset_status_t render_settings_read(const asset_pack_t* pack, render_settings_t* out);
