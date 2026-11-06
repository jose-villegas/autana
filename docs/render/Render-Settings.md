# Render Settings

A render context draws with one settings value, `render_settings_t`: the
renderer's quality and cost choices. An app authors it as a
[render asset](#the-render-asset) its pack holds, which the shell applies as
the app starts, and may set it with `render_context_set_settings()` on any
frame; the next frame draws with it.

In a development build each setting also has a `TUNE` knob
([Live-Tuning.md](../cli/Live-Tuning.md)). A knob that is set overrides the
app's value for every frame, without writing into it. `RESET` clears the
override, and the app's value comes back.

```mermaid
flowchart LR
    Asset["the app's pack: entry render<br/>set by the shell at start"] --> Settings
    App["app<br/>render_context_set_settings()"] --> Settings["the context's settings<br/>render_context_settings() reads them back"]
    Settings --> Pick{"knob set?<br/>development builds only"}
    Knob["TUNE render.cull,<br/>render.depth_fit, render.batched"] -->|SET| Pick
    Knob -->|RESET| Unset["unset"]
    Unset --> Pick
    Pick -->|"no"| Frame["the frame draws with<br/>raster_settings()"]
    Pick -->|"yes: the knob's value"| Frame
```

A release build has no knobs, so it draws with the app's settings only. A raster
drawn without a context uses `RENDER_SETTINGS_DEFAULT`.

## The settings

Basic settings are for any app to weigh. Advanced ones need the renderer's
workings to judge. Every default is on.

<!-- generated: render-settings check: python launcher/tools/render/settings/settings_table.py --check -->
| Field | Default | Tier | What it trades |
|---|---|---|---|
| `cull` | `true` | basic | skips clusters and world boxes outside the picture |
| `depth_fit` | `true` | advanced | depth steps go where the kept scene is, not to near_z; needs cull |
| `batched` | `true` | advanced | one transform and one draw fork/join a batch, not an instance |
<!-- /generated: render-settings -->

## cull

Culling keeps only the clusters, and the instances, that can show in the
picture ([every stage](frame/Culling.md)). It changes no pixel, so the figure
is what it saves. The chase below marks each cluster's fate along Bistro's path:

![Bistro's fates from behind the camera](../images/render/cull-bistro-chase.gif)

Mean frame stages along Sponza's camera path on the board, culling on and off:

<!-- generated: render-settings-cull sha256=5d303133eed2f3851202b76f0e87e9bce6bc029ce57cb2cc3151f5e402ec8c34 -->
| Setting | Cull ms | Transform ms | Draw ms | Frame ms |
|---|---|---|---|---|
| `cull` on | 0.60 | 3.24 | 33.89 | 40.57 |
| `cull` off | 0.34 | 5.70 | 48.46 | 57.28 |
| off - on | -0.26 | 2.46 | 14.56 | 16.71 |
<!-- /generated: render-settings-cull -->

The cull stage itself costs more with culling on; the saving is in the
transform and the draw. Off is only for measuring.

## depth_fit

Depth is stored in 16 bits as `depth_near / z`. With `depth_fit` on, each
frame scales it to the nearest bound it kept, not the clip plane's `near_z`
([Attachments.md](frame/Attachments.md#depth)). Seen from high up, the scale
then gives distant surfaces many more steps.

With `depth_fit` off, a patch of ground just under Bistro's street shows through the road.
Below is one frame with each setting, and their difference:

![One Bistro frame with depth scaled by near_z and by the fit, and their difference](../images/render/depth-fit-bistro-diff.png)

The places they differ most, enlarged:

![The places the two settings differ most](../images/render/depth-fit-bistro-diff.crops.png)

The artefact moves from frame to frame:

![Consecutive frames of one window, near_z on top and the fit below](../images/render/depth-fit-bistro.gif)

The fit adds no pass of its own: the cull's walk tracks the nearest bound it
keeps. So `depth_fit` has an effect only with `cull` on.

## batched

Instances draw in batches of consecutive instances whose vertices and
clusters fit the scratch sized for the largest mesh, up to
`RASTER_BATCH_INSTANCES`. Each batch takes one transform fork/join and one
draw fork/join, where each instance took two. It changes no pixel.

Mean and p95 frame time along each camera path on the board, batching on and
off ([the captures](data/batched-board.log)):

<!-- generated: render-settings-batched check: python launcher/tools/render/settings/batched_cost.py --check -->
| Scene | Quarter | Mean ms, on | Mean ms, off | Off - on | p95 ms, on | p95 ms, off |
|---|---|---|---|---|---|---|
| bistro-fitted-full | landscape | 37.5 | 40.1 | 2.7 | 51.1 | 53.5 |
| bistro-fitted-full | portrait | 35.8 | 38.5 | 2.7 | 53.8 | 56.4 |
| sponza-fitted-full | landscape | 45.4 | 49.2 | 3.7 | 55.4 | 59.2 |
| sponza-fitted-full | portrait | 48.2 | 51.9 | 3.7 | 59.4 | 63.0 |
<!-- /generated: render-settings-batched -->

Off is only for measuring.

## The render asset

Settings are authored in a `NAME.render.toml`, one key per field. The engine's
is [`default.render.toml`](../../launcher/main/render/settings/default.render.toml),
and `RENDER_SETTINGS_DEFAULT` is generated from it
([`render_asset.py`](../../launcher/tools/render/settings/render_asset.py)).
Any other file lists only the values it changes; the rest come from the
default. A key that is no field, or a value of another type, fails the build.

```toml
# fast.render.toml: the default, but drawn one instance at a time
batched = false
```

An app's `.pack.toml` names one with `render = "PATH"`, relative to itself
([Building-Packs.md](../assets/Building-Packs.md#packs)), and the pack holds it
as entry `render`. The shell mounts the pack an app names in `app_t.pack` and,
before `enter()`, sets the main context's settings from that entry, or to
`RENDER_SETTINGS_DEFAULT` without one. So no app starts with the last app's
settings. Scenes never carry render settings.

```mermaid
flowchart LR
    Default["default.render.toml"] -->|render_asset.py| Header["RENDER_SETTINGS_DEFAULT<br/>render_settings_default.h"]
    File["NAME.render.toml"] -->|"build_pack.py<br/>missing keys from the default"| Entry["the app's pack:<br/>TABL entry render"]
    Default --> Entry
    Entry -->|"shell, at app start<br/>render_settings_read()"| Context["render_context_set_settings()<br/>on the main context"]
    Header -->|"pack without the entry"| Context
```

The entry is a [TABL v1](../assets/formats/TABL.md) table, one value a row:

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 84
---
packet-beta
title Header, 8 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "rows = fields"
4-5: "columns = 1"
6: "frac = 0"
7: "0"
```

```mermaid
---
config:
  packet:
    bitsPerRow: 13
    bitWidth: 56
---
packet-beta
title A row, 26 bytes from byte 8, one per field
0-23: "field name, NUL padded"
24-25: "int16 value"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 8 bytes |
| Rows | `8 + i * 26` | 26 bytes a row, one per field in struct order: 86 bytes for three fields |

| Rule | Detail |
|---|---|
| Owners | `RENDER_SETTINGS_ENTRY` and the fields in `render_settings.h`; the table layout in `asset_table_format.h` |
| Rows | exactly one per `render_settings_t` field, named by it; a `bool` is 0 or 1 |
| Errors | `render_settings_read()`: `ASSET_ERR_NOT_FOUND` without the entry; `ASSET_ERR_FORMAT` for another row count, a missing field, more than one column or a nonzero `frac`; TABL's own errors. `out` is written only on `ASSET_OK` |
| Failure at start | the shell logs it and starts the app with `RENDER_SETTINGS_DEFAULT` |
