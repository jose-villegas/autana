/* GENERATED FILE - do not edit.
 *     python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h
 */
#pragma once

#define RENDER_SETTINGS_DEFAULT ((render_settings_t){.cull = true, .depth_fit = true, .batched = true, .partial_redraw = true, .lod = {.tolerance_px = 2.0F, .hysteresis_px = 0.5F, .coverage_permille = 250, .pick_every = 4, .triangle_budget = 0, .blend = 0, .blend_px = 0.25F}, .anim = {.mid_px = 48.0F, .far_px = 20.0F, .mid_every = 2, .far_every = 4, .hysteresis_px = 4.0F}, .texture = {.divide_px = 16, .affine_bend = 1.0F, .affine_level = 1}, .small_fill = {.far_z = 0.0F, .far_ceiling = 0}})
/* X(field, type, valid) in struct order; valid tests the typed field as `value`. */
#define RENDER_SETTINGS_FIELDS(X) X(cull, bool, true) X(depth_fit, bool, true) X(batched, bool, true) X(partial_redraw, bool, true) X(lod.tolerance_px, float, true) X(lod.hysteresis_px, float, true) X(lod.coverage_permille, int, true) X(lod.pick_every, int, true) X(lod.triangle_budget, int, true) X(lod.blend, int, value >= 0 && value < RENDER_LOD_BLEND_COUNT) X(lod.blend_px, float, value > 0.0F && value <= RENDER_LOD_BLEND_PX_MAX) X(anim.mid_px, float, value >= 0.0F && value <= RENDER_ANIM_PIXELS_MAX) X(anim.far_px, float, value >= 0.0F && value <= RENDER_ANIM_PIXELS_MAX) X(anim.mid_every, int, value >= RENDER_ANIM_EVERY_MIN && value <= RENDER_ANIM_EVERY_MAX) X(anim.far_every, int, value >= RENDER_ANIM_EVERY_MIN && value <= RENDER_ANIM_EVERY_MAX) X(anim.hysteresis_px, float, value >= 0.0F && value <= RENDER_ANIM_PIXELS_MAX) X(texture.divide_px, int, value >= 0 && value <= RENDER_TEXTURE_DIVIDE_PX_MAX) X(texture.affine_bend, float, value >= 0.0F && value <= RENDER_TEXTURE_BEND_MAX) X(texture.affine_level, int, value >= 0 && value <= RENDER_TEXTURE_LEVEL_MAX) X(small_fill.far_z, float, value >= 0.0F) X(small_fill.far_ceiling, int, value >= 0 && value <= RENDER_SMALL_FILL_CEILING_MAX)
#define RENDER_SETTINGS_FIELD_COUNT 21
#define RENDER_SETTINGS_BIT_CULL (UINT32_C(1) << 0)
#define RENDER_SETTINGS_BIT_DEPTH_FIT (UINT32_C(1) << 1)
#define RENDER_SETTINGS_BIT_BATCHED (UINT32_C(1) << 2)
#define RENDER_SETTINGS_BIT_PARTIAL_REDRAW (UINT32_C(1) << 3)
#define RENDER_SETTINGS_BIT_LOD_TOLERANCE_PX (UINT32_C(1) << 4)
#define RENDER_SETTINGS_BIT_LOD_HYSTERESIS_PX (UINT32_C(1) << 5)
#define RENDER_SETTINGS_BIT_LOD_COVERAGE_PERMILLE (UINT32_C(1) << 6)
#define RENDER_SETTINGS_BIT_LOD_PICK_EVERY (UINT32_C(1) << 7)
#define RENDER_SETTINGS_BIT_LOD_TRIANGLE_BUDGET (UINT32_C(1) << 8)
#define RENDER_SETTINGS_BIT_LOD_BLEND (UINT32_C(1) << 9)
#define RENDER_SETTINGS_BIT_LOD_BLEND_PX (UINT32_C(1) << 10)
#define RENDER_SETTINGS_BIT_ANIM_MID_PX (UINT32_C(1) << 11)
#define RENDER_SETTINGS_BIT_ANIM_FAR_PX (UINT32_C(1) << 12)
#define RENDER_SETTINGS_BIT_ANIM_MID_EVERY (UINT32_C(1) << 13)
#define RENDER_SETTINGS_BIT_ANIM_FAR_EVERY (UINT32_C(1) << 14)
#define RENDER_SETTINGS_BIT_ANIM_HYSTERESIS_PX (UINT32_C(1) << 15)
#define RENDER_SETTINGS_BIT_TEXTURE_DIVIDE_PX (UINT32_C(1) << 16)
#define RENDER_SETTINGS_BIT_TEXTURE_AFFINE_BEND (UINT32_C(1) << 17)
#define RENDER_SETTINGS_BIT_TEXTURE_AFFINE_LEVEL (UINT32_C(1) << 18)
#define RENDER_SETTINGS_BIT_SMALL_FILL_FAR_Z (UINT32_C(1) << 19)
#define RENDER_SETTINGS_BIT_SMALL_FILL_FAR_CEILING (UINT32_C(1) << 20)
