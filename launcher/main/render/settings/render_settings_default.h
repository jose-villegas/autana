/* GENERATED FILE - do not edit.
 *     python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h
 */
#pragma once

#define RENDER_SETTINGS_DEFAULT ((render_settings_t){.cull = true, .depth_fit = true, .batched = true, .partial_redraw = true, .lod = {.tolerance_px = 2.0F, .hysteresis_px = 0.5F, .coverage_permille = 250, .pick_every = 4, .triangle_budget = 0}, .anim = {.mid_px = 48.0F, .far_px = 20.0F, .mid_every = 2, .far_every = 4, .hysteresis_px = 4.0F}})
/* X(field, type) for each field of render_settings_t, in order: `field` as a member path. */
#define RENDER_SETTINGS_FIELDS(X) X(cull, bool) X(depth_fit, bool) X(batched, bool) X(partial_redraw, bool) X(lod.tolerance_px, float) X(lod.hysteresis_px, float) X(lod.coverage_permille, int) X(lod.pick_every, int) X(lod.triangle_budget, int) X(anim.mid_px, float) X(anim.far_px, float) X(anim.mid_every, int) X(anim.far_every, int) X(anim.hysteresis_px, float)
#define RENDER_SETTINGS_FIELD_COUNT 14
