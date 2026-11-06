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
    bool cull;      /* basic: skips clusters and world boxes outside the picture */
    bool depth_fit; /* advanced: depth steps go where the kept scene is, not to near_z; needs cull */
    bool batched;   /* advanced: one transform and one draw fork/join a batch, not an instance */
} render_settings_t;

/* RENDER_SETTINGS_DEFAULT, from default.render.toml beside this header. */
#include "render/settings/render_settings_default.h"

/* The pack entry a .pack.toml's `render` key bakes: a TABL with one row per field. */
#define RENDER_SETTINGS_ENTRY "render"

/* Reads `pack`'s RENDER_SETTINGS_ENTRY: a row for every field and no other.
 * ASSET_ERR_NOT_FOUND when the pack has none; `out` is written only on ASSET_OK. */
asset_status_t render_settings_read(const asset_pack_t* pack, render_settings_t* out);
