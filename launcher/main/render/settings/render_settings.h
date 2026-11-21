/*
 * render_settings: the quality and cost choices a render context draws with,
 * one value an app sets in one call (render_context_set_settings()) or
 * authors as a NAME.render.toml its pack names. A development override
 * (TUNE render.cull and the others) goes on top of it without writing into it.
 */
#pragma once

#include <stdbool.h>

#include "asset/asset_pack.h"

/* Each field, then its tier, the interval a pack may set it in when it has
 * one, and what it trades: render_asset.py and docs/render/Render-Settings.md
 * read these lines, one field each, so the formatter leaves them whole.
 * Basic is for any app; advanced needs the renderer's workings to judge. */
// clang-format off
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
/* The most frames lod.pick_every may spread the pick over. */
#define RENDER_LOD_PICK_EVERY_MAX 8

typedef struct {
    float tolerance_px;  /* basic: the projected error a group may have and still draw coarse; more saves, pops more */
    float hysteresis_px; /* advanced: how far past the tolerance a coarse group's error goes before it turns fine */
    int coverage_permille; /* basic: the thousandths of the picture's height a coarse group may span; smaller pops less */
    int pick_every; /* advanced [1, RENDER_LOD_PICK_EVERY_MAX]: the frames the pick is spread over, a scene's first frame picking all; more saves, switches later */
    int triangle_budget; /* basic: the triangles a frame aims to draw, the tolerance rising past tolerance_px to keep within; 0 never rises */
    int blend; /* basic [0, RENDER_LOD_BLEND_COUNT): 0 switches at once; 1 blends each switch, by morph where a mesh was imported with geometry.morph, else by dither */
    float blend_px; /* advanced (0.0, RENDER_LOD_BLEND_PX_MAX]: the error past a switch over which a blend fades out; longer is softer, moves more */
} render_lod_settings_t;

#define RENDER_ANIM_EVERY_MIN  1
#define RENDER_ANIM_EVERY_MAX  4
#define RENDER_ANIM_PIXELS_MAX 4096

typedef struct {
    float mid_px;        /* basic [0.0, RENDER_ANIM_PIXELS_MAX]: projected radius in pixels below which a copy is posed every mid_every frames */
    float far_px;        /* basic [0.0, RENDER_ANIM_PIXELS_MAX]: projected radius in pixels below which a copy is posed every far_every frames */
    int mid_every;       /* basic [RENDER_ANIM_EVERY_MIN, RENDER_ANIM_EVERY_MAX]: frames between mid-sized poses; 1 poses every frame */
    int far_every;       /* basic [RENDER_ANIM_EVERY_MIN, RENDER_ANIM_EVERY_MAX]: frames between far poses; 1 poses every frame */
    float hysteresis_px; /* advanced [0.0, RENDER_ANIM_PIXELS_MAX]: pixels past a tier boundary before switching */
} render_anim_settings_t;

/* The longest texture.divide_px a pack may set, and the longest a segment
 * grows to: an 8.8 step across it drifts under an eighth of a texel. */
#define RENDER_TEXTURE_DIVIDE_PX_MAX 32
/* The largest texture.affine_bend a pack may set, in texels. */
#define RENDER_TEXTURE_BEND_MAX      15.0F
/* Steps per texel in which the span target and the tune override hold
 * texture.affine_bend, rounded. */
#define RENDER_TEXTURE_BEND_STEPS    16
/* The last mip level a texture has: the largest level setting a pack may name. */
#define RENDER_TEXTURE_LEVEL_MAX     7

typedef struct {
    int divide_px; /* basic [0, RENDER_TEXTURE_DIVIDE_PX_MAX]: fewest pixels between divides by depth, fewer cost more; 0 is affine, near surfaces bend */
    float affine_bend; /* advanced [0.0, RENDER_TEXTURE_BEND_MAX]: texels a triangle may bend and still step affine; larger is faster, bends more */
    int affine_level; /* advanced [0, RENDER_TEXTURE_LEVEL_MAX]: first mip level that steps affine, its pattern shifting rather than bending; 0 lets every level divide */
    int far_level; /* basic [0, RENDER_TEXTURE_LEVEL_MAX]: first mip level drawn untextured, one colour a triangle; lower is faster; 0 textures every level */
} render_texture_settings_t;

/* small_fill.style: the colour kernel above the always-flat side. */
typedef enum { SMALL_FILL_FLAT, SMALL_FILL_GRAIN, SMALL_FILL_DITHER, SMALL_FILL_STYLE_COUNT } small_fill_style_t;

#define SMALL_FILL_CEILING_MAX    16
#define SMALL_FILL_CEILING_OFF    0 /* the ceiling with no small fill */
#define SMALL_FILL_GATE_MAX       255 /* an RGB565 channel's spread fits a byte */
#define SMALL_FILL_CEILING_SMOOTH 4
#define SMALL_FILL_GATE_SMOOTH    0
#define SMALL_FILL_CEILING_FAST   12
#define SMALL_FILL_GATE_FAST      16

typedef struct {
    int ceiling; /* basic [0, SMALL_FILL_CEILING_MAX]: largest bounding-box side in pixel centres filled in one colour; 0 is off; more saves, blurs small detail */
    int gate; /* advanced [0, SMALL_FILL_GATE_MAX]: RGB565 per-channel spread, in packed channel units, a box at the ceiling may span and still fill; 0 lets any */
    int style; /* advanced [0, SMALL_FILL_STYLE_COUNT): 0 flat, 1 grain, 2 dither (a small_fill_style_t): how a filled box above the always-flat side is coloured */
} render_small_fill_settings_t;

#define SMALL_FILL_SMOOTH      ((render_small_fill_settings_t){SMALL_FILL_CEILING_SMOOTH, SMALL_FILL_GATE_SMOOTH, SMALL_FILL_FLAT})
#define SMALL_FILL_FAST        ((render_small_fill_settings_t){SMALL_FILL_CEILING_FAST, SMALL_FILL_GATE_FAST, SMALL_FILL_FLAT})
#define SMALL_FILL_FAST_DITHER ((render_small_fill_settings_t){SMALL_FILL_CEILING_FAST, SMALL_FILL_GATE_FAST, SMALL_FILL_DITHER})
#define SMALL_FILL_FAST_GRAIN  ((render_small_fill_settings_t){SMALL_FILL_CEILING_FAST, SMALL_FILL_GATE_FAST, SMALL_FILL_GRAIN})

/* Every SMALL_FILL_<name> preset, for a list that has to cover them all: X(name) each. */
#define SMALL_FILL_PRESETS(X) X(SMOOTH) X(FAST) X(FAST_DITHER) X(FAST_GRAIN)

typedef struct {
    bool cull;                   /* basic: skips clusters and world boxes outside the picture */
    bool depth_fit;              /* advanced: depth steps go where the kept scene is, not to near_z; needs cull */
    bool batched;                /* advanced: one transform and one draw fork/join a batch, not an instance */
    bool partial_redraw;         /* basic: a still camera redraws only the rows that moved; the picture is the same */
    render_lod_settings_t lod;   /* the level pick (docs/render/frame/Levels-of-Detail.md) */
    render_anim_settings_t anim; /* pose rate by size (docs/render/animation/Skinned-Meshes.md) */
    render_texture_settings_t texture; /* the textured span fill (docs/assets/formats/LTEX.md) */
    render_small_fill_settings_t small_fill; /* small triangles filled in one colour (docs/render/frame/Coverage.md#small-fill) */
} render_settings_t;

// clang-format on

/* RENDER_SETTINGS_DEFAULT, from default.render.toml beside this header. */
#include "render/settings/render_settings_default.h"

/* The pack entry a .pack.toml's `render` key bakes: a TABL with one row per field,
 * a float in 1/2^RENDER_SETTINGS_FRAC_BITS steps, a bool or an int as itself. */
#define RENDER_SETTINGS_ENTRY     "render"
#define RENDER_SETTINGS_FRAC_BITS 4

/* Reads `pack`'s RENDER_SETTINGS_ENTRY: a row for every field and no other,
 * each within the interval its field comment gives (else ASSET_ERR_FORMAT). ASSET_ERR_NOT_FOUND when
 * the pack has none; `out` is written only on ASSET_OK. */
asset_status_t render_settings_read(const asset_pack_t* pack, render_settings_t* out);

/* `set` under every development override that is set; a release build's is
 * `set`. A render context resolves it once a frame, and the scene once a repose,
 * so a draw or a pose reads plain fields. */
render_settings_t render_settings_tuned(const render_settings_t* set);

/* `anim` under the anim overrides, in place: render_settings_tuned()'s tiers,
 * for a caller holding only them. */
void render_anim_settings_tune(render_anim_settings_t* anim);
