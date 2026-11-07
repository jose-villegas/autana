/* GENERATED FILE - do not edit.
 *     python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h
 */
#pragma once

#define RENDER_SETTINGS_DEFAULT ((render_settings_t){.cull = true, .depth_fit = true, .batched = true, .partial_redraw = false, .lod = {.tolerance_px = 2.0F, .hysteresis_px = 0.5F, .coverage_permille = 250}})
/* X(field, type) for each field of render_settings_t, in order: `field` as a member path. */
#define RENDER_SETTINGS_FIELDS(X) X(cull, bool) X(depth_fit, bool) X(batched, bool) X(partial_redraw, bool) X(lod.tolerance_px, float) X(lod.hysteresis_px, float) X(lod.coverage_permille, int)
#define RENDER_SETTINGS_FIELD_COUNT 7
