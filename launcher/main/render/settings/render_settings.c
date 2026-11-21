#include "render/settings/render_settings.h"

#include "asset/asset_table.h"
#include "asset/asset_table_format.h"
#include "core/tune.h"
#include "math/scalar/mathi.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_span.h"

asset_status_t
render_settings_read(const asset_pack_t* pack, render_settings_t* out) {
    asset_view_t entry;
    asset_table_t table;
    asset_status_t status = asset_pack_find(pack, RENDER_SETTINGS_ENTRY, ASSET_TABLE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = asset_table_open(entry, &table);
    }
    if (status == ASSET_OK
        && (table.rows != RENDER_SETTINGS_FIELD_COUNT || table.frac_bits != RENDER_SETTINGS_FRAC_BITS)) {
        status = ASSET_ERR_FORMAT;
    }
    render_settings_t read = {0};
    const int16_t* encoded = NULL;
#define FROM_bool(v)  ((v) != 0)
#define FROM_float(v) ((float)(v) / (float)(1 << RENDER_SETTINGS_FRAC_BITS))
#define FROM_int(v)   ((int)(v))
#define READ_FIELD(field, type, valid)                                                                                 \
    if (status == ASSET_OK) {                                                                                          \
        status = asset_table_row(&table, #field, 1, &encoded) == ASSET_OK ? ASSET_OK : ASSET_ERR_FORMAT;               \
    }                                                                                                                  \
    if (status == ASSET_OK) {                                                                                          \
        read.field = FROM_##type(*encoded);                                                                            \
        const type value = read.field;                                                                                 \
        (void)value;                                                                                                   \
        if (!(valid)) {                                                                                                \
            status = ASSET_ERR_FORMAT;                                                                                 \
        }                                                                                                              \
    }
    RENDER_SETTINGS_FIELDS(READ_FIELD)
    if (status == ASSET_OK) {
        *out = read;
    }
    return status;
}

TUNE_OWNER(render);
/* Development overrides of the settings a context draws and the scene poses with (TUNE_OVERRIDE). */
/* The generated meshlet-size table measures both culling settings. */
TUNE_OVERRIDE(render, cull, 1);
/* 0 encodes depth with the clip plane's near_z: the depth-precision figures
 * render both. */
TUNE_OVERRIDE(render, depth_fit, 1);
/* Off draws one instance a batch. */
TUNE_OVERRIDE(render, batched, 1);
/* Set, overrides the app's partial redraw setting either way: the board compares both. */
TUNE_OVERRIDE(render, partial_redraw, 1);
/* Overrides lod.coverage_permille (INT32_MAX lifts the cap); coverage_table.py
 * sets it once per cap. */
TUNE_OVERRIDE(render, lod_coverage_permille, INT32_MAX);
/* Overrides lod.blend; morph_table.py sets it once per arm. */
TUNE_OVERRIDE(render, lod_blend, RENDER_LOD_BLEND_COUNT - 1);
/* Overrides lod.blend_px in hundredths of a pixel; morph_table.py sweeps it. */
#define LOD_BLEND_CENTI 100
TUNE_OVERRIDE(render, lod_blend_centipixels, (int32_t)(RENDER_LOD_BLEND_PX_MAX * LOD_BLEND_CENTI));
/* Overrides lod.pick_every; 1 picks every instance every frame. */
TUNE_OVERRIDE(render, lod_pick_every, R3D_LOD_PICK_EVERY_MAX);
/* Overrides lod.triangle_budget. */
TUNE_OVERRIDE(render, lod_triangle_budget, INT32_MAX);
/* Override texture.divide_px, texture.affine_bend in 1/R3D_SPAN_BEND_STEPS
 * texel and texture.affine_level; texture_figures.py sets all three for its
 * true divide. */
TUNE_OVERRIDE(render, texture_divide_px, RENDER_TEXTURE_DIVIDE_PX_MAX);
TUNE_OVERRIDE(render, texture_bend_steps, (int32_t)(RENDER_TEXTURE_BEND_MAX * R3D_SPAN_BEND_STEPS));
TUNE_OVERRIDE(render, texture_affine_level, RENDER_TEXTURE_LEVEL_MAX);
/* Overrides texture.far_level; 0 reads every level's texels. */
TUNE_OVERRIDE(render, texture_far_level, RENDER_TEXTURE_LEVEL_MAX);
/* Overrides settle.frame_us; 0 never refines a held view. */
TUNE_OVERRIDE(render, settle_frame_us, RENDER_SETTLE_FRAME_US_MAX);

TUNE_OWNER(anim);
/* Override the pose-rate tiers in whole pixels and frames; an interval below
 * the minimum poses every frame. */
TUNE_OVERRIDE(anim, mid_px, RENDER_ANIM_PIXELS_MAX);
TUNE_OVERRIDE(anim, far_px, RENDER_ANIM_PIXELS_MAX);
TUNE_OVERRIDE(anim, mid_every, RENDER_ANIM_EVERY_MAX);
TUNE_OVERRIDE(anim, far_every, RENDER_ANIM_EVERY_MAX);
TUNE_OVERRIDE(anim, hysteresis_px, RENDER_ANIM_PIXELS_MAX);

static float
anim_pixels(int32_t knob, float value) {
    return knob == TUNE_UNSET ? value : (float)mathi_clamp(knob, 0, RENDER_ANIM_PIXELS_MAX);
}

static int
anim_every(int32_t knob, int value) {
    return mathi_clamp(tune_override(knob, value), RENDER_ANIM_EVERY_MIN, RENDER_ANIM_EVERY_MAX);
}

render_settings_t
render_settings_tuned(const render_settings_t* set) {
    const render_settings_t s = *set;
    render_settings_t tuned = {
        tune_override(cull, s.cull) != 0,
        tune_override(depth_fit, s.depth_fit) != 0,
        tune_override(batched, s.batched) != 0,
        tune_override(partial_redraw, s.partial_redraw) != 0,
        {s.lod.tolerance_px, s.lod.hysteresis_px, tune_override(lod_coverage_permille, s.lod.coverage_permille),
         tune_override(lod_pick_every, s.lod.pick_every), tune_override(lod_triangle_budget, s.lod.triangle_budget),
         tune_override(lod_blend, s.lod.blend),
         lod_blend_centipixels == TUNE_UNSET ? s.lod.blend_px : (float)lod_blend_centipixels / LOD_BLEND_CENTI},
        s.anim,
        {tune_override(texture_divide_px, s.texture.divide_px),
         texture_bend_steps == TUNE_UNSET ? s.texture.affine_bend : (float)texture_bend_steps / R3D_SPAN_BEND_STEPS,
         tune_override(texture_affine_level, s.texture.affine_level),
         tune_override(texture_far_level, s.texture.far_level)},
        s.small_fill,
        {tune_override(settle_frame_us, s.settle.frame_us), s.settle.hold_frames}};
    render_anim_settings_tune(&tuned.anim);
    return tuned;
}

void
render_anim_settings_tune(render_anim_settings_t* anim) {
    anim->mid_px = anim_pixels(mid_px, anim->mid_px);
    anim->far_px = anim_pixels(far_px, anim->far_px);
    anim->mid_every = anim_every(mid_every, anim->mid_every);
    anim->far_every = anim_every(far_every, anim->far_every);
    anim->hysteresis_px = anim_pixels(hysteresis_px, anim->hysteresis_px);
}
