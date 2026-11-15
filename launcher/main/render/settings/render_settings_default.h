/* GENERATED FILE - do not edit.
 *     python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h
 */
#pragma once

#define RENDER_SETTINGS_DEFAULT ((render_settings_t){.cull = true, .depth_fit = true, .batched = true, .partial_redraw = true, .lod = {.tolerance_px = 2.0F, .hysteresis_px = 0.5F, .coverage_permille = 250, .pick_every = 4, .triangle_budget = 0, .blend = 0, .blend_px = 0.25F}, .anim = {.mid_px = 48.0F, .far_px = 20.0F, .mid_every = 2, .far_every = 4, .hysteresis_px = 4.0F}})
/* X(field, type, valid) in struct order; valid tests the typed field as `value`. */
#define RENDER_SETTINGS_FIELDS(X) X(cull, bool, true) X(depth_fit, bool, true) X(batched, bool, true) X(partial_redraw, bool, true) X(lod.tolerance_px, float, true) X(lod.hysteresis_px, float, true) X(lod.coverage_permille, int, true) X(lod.pick_every, int, true) X(lod.triangle_budget, int, true) X(lod.blend, int, value >= 0 && value < RENDER_LOD_BLEND_COUNT) X(lod.blend_px, float, value > 0.0F && value <= RENDER_LOD_BLEND_PX_MAX) X(anim.mid_px, float, value >= 0.0F && value <= RENDER_ANIM_PIXELS_MAX) X(anim.far_px, float, value >= 0.0F && value <= RENDER_ANIM_PIXELS_MAX) X(anim.mid_every, int, value >= RENDER_ANIM_EVERY_MIN && value <= RENDER_ANIM_EVERY_MAX) X(anim.far_every, int, value >= RENDER_ANIM_EVERY_MIN && value <= RENDER_ANIM_EVERY_MAX) X(anim.hysteresis_px, float, value >= 0.0F && value <= RENDER_ANIM_PIXELS_MAX)
#define RENDER_SETTINGS_FIELD_COUNT 16
