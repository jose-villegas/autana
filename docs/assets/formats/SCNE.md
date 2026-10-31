# SCNE v5 scene

One scene: its entities, their transforms and parents, and the components on them ([Scene-Files.md](../../render/scene/Scene-Files.md) is the authored file, [Scene-Manager.md](../../render/scene/Scene-Manager.md) the runtime). Layout defines: [`scene/scene_format.h`](../../../launcher/main/scene/scene_format.h); row structs in [`scene/scene_internal.h`](../../../launcher/main/scene/scene_internal.h).

`build_pack.py` bakes `NAME.scene.toml` into entry `NAME` of its own pack `NAME`, beside every mesh, skin, skeleton and clip it names; a scene and its clip cannot share a stem ([Building-Packs.md](../Building-Packs.md#packs)). `scene_load()` opens it and each entry it names, and fails naming whichever is missing or malformed.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(8) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(4) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(3) rect:nth-of-type(5) { fill: #f4a885 !important; } g:nth-of-type(3) rect:nth-of-type(6) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(4) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(4) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 64
---
packet-beta
title Header, 64 bytes at 0, one cell a byte
0-1: "version = 5"
2-3: "names"
4-5: "transforms"
6-7: "renderers"
8-9: "cameras"
10-11: "lights"
12-13: "skins"
14-15: "animators"
16-17: "clips"
18-19: "settings"
20-21: "parents"
22-23: "0"
24-27: "names_off"
28-31: "transforms_off"
32-35: "renderers_off"
36-39: "cameras_off"
40-43: "lights_off"
44-47: "skins_off"
48-51: "animators_off"
52-55: "clips_off"
56-59: "settings_off"
60-63: "parents_off"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 18
    bitWidth: 44
---
packet-beta
title Name registry, one 36-byte row an entity, from names_off
0-3: "u32 name_hash"
4-35: "char name[32], NUL padded"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Content: transform, 48 bytes an entity from transforms_off
0-35: "f32 linear[3][3], rotation times scale"
36-47: "f32 position[3]"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 18
    bitWidth: 44
---
packet-beta
title Content: renderer, 36 bytes from renderers_off
0-1: "entity"
2-3: "colour_set"
4-35: "char mesh_id[32]"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Content: camera, 16 bytes from cameras_off
0-1: "entity"
2-3: "0"
4-7: "f32 half_fov_short_tan"
8-11: "f32 near_z"
12-15: "u32 clear_rgb"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 20
    bitWidth: 44
---
packet-beta
title Content: light, 20 bytes from lights_off
0-1: "entity"
2: "kind"
3: "0"
4-15: "f32 colour[3], linear"
16-19: "f32 intensity"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 20
    bitWidth: 44
---
packet-beta
title Content: skinned renderer, 100 bytes from skins_off
0-1: "entity"
2-3: "0"
4-35: "char mesh_id[32]"
36-67: "char skin_id[32]"
68-99: "char skeleton_id[32]"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 44
---
packet-beta
title Content: animator, 12 bytes from animators_off
0-1: "entity"
2-3: "clip_count"
4-7: "first_clip"
8-9: "default_clip"
10: "wrap"
11: "0"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Clip registry, one 32-byte row a clip id, from clips_off
0-31: "char clip_id[32], NUL padded"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 20
    bitWidth: 44
---
packet-beta
title Content: settings, 20 bytes at settings_off, exactly one
0-11: "f32 ambient_colour[3], linear"
12-15: "f32 ambient_intensity"
16-19: "f32 tonemap_white"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 4
    bitWidth: 44
---
packet-beta
title Content: parents, 2 bytes an entity from parents_off
0-1: "parent"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 64 bytes: ten u16 counts from `SCENE_ASSET_AT_COUNTS`, then ten u32 offsets from `SCENE_ASSET_AT_OFFSETS`, both in `scene_asset_part_t` order |
| Name registry | `names_off` | 36 bytes an entity |
| Content: transforms | `transforms_off` | 48 bytes an entity; the count equals the names' |
| Content: renderers | `renderers_off` | 36 bytes each |
| Content: cameras | `cameras_off` | 16 bytes each |
| Content: lights | `lights_off` | 20 bytes each |
| Content: skinned renderers | `skins_off` | 100 bytes each |
| Content: animators | `animators_off` | 12 bytes each |
| Clip registry | `clips_off` | 32 bytes a clip id, indexed by `first_clip` and `clip_count` |
| Content: settings | `settings_off` | 20 bytes, one row |
| Content: parents | `parents_off` | 2 bytes an entity; the count equals the names'; written for every scene, all roots included |

| Rule | Detail |
|---|---|
| Owners | `SCENE_ASSET_VERSION`, `_HEADER_SIZE` (64), `_AT_COUNTS`, `_AT_OFFSETS`, `SCENE_ASSET_DEFAULT_CLIP_NONE` (0xFFFF), `SCENE_ASSET_PARENT_ROOT` (0xFFFF), `SCENE_ASSET_COLOUR_SET_OWN` (0xFFFF), `SCENE_LIGHT_KIND_DIRECTIONAL` (1), `SCENE_ASSET_NAME_SIZE` (36), `_NAME_AT_HASH` (0), `_NAME_AT_TEXT` (4) in `scene_format.h`; row structs in `scene_internal.h`, checked against the sizes above by `_Static_assert` |
| Layout | every part offset is a multiple of 4, at least 64, and the part ends inside the entry; the writer lays parts out in header order, each padded to 4 bytes |
| Counts | each at most 65535, entities at most 65534; names equals transforms and parents; settings is exactly 1 |
| Names | letters, digits and `_`, starting with a letter or `_`, at most 31 bytes; `name_hash` is `name_hash(name)` ([hash package](../../../launcher/packages/hash/README.md)) and never `NAME_HASH_NONE`, and finds compare it alone. The bake refuses a name that hashes to `NAME_HASH_NONE` and two names in one scene that share a hash, naming both |
| Transforms | in the parent's frame. `scene_load()` refuses a shear and names its entity; a reflection (determinant −1) loads as a negative x scale and draws mirrored |
| Parents | `parent` is `SCENE_ASSET_PARENT_ROOT` or below its own entity, so one forward pass meets every parent before its children. A camera, light or skinned renderer is a root that does not mirror. An entity with children has a uniform scale, the same on every axis and not mirrored, and `scene_load()` refuses one that does not, naming it; `scene_asset.encode()` folds a parent's uneven scale into its children, refusing a parent that draws or a turned child the fold would shear. A mesh bundle's copies are children of the renderer that carries them ([MCOP](MCOP.md)). Transform and enabled state carry to descendants ([Scene-Manager.md](../../render/scene/Scene-Manager.md#hierarchy)) |
| Entities | the scene's objects in file order; every `entity` is below the names' count; at most one light, one skinned renderer and one animator an entity |
| Colour sets | own colours (`SCENE_ASSET_COLOUR_SET_OWN`) need no LCOL, checked at pack time, and authored scenes write own colours; [Selection and ownership](LCOL.md) |
| Text | `name`, `mesh_id`, `skin_id`, `skeleton_id` and clip ids are nonempty, NUL terminated, zero after the NUL |
| Camera | `half_fov_short_tan` and `near_z` finite and positive; `clear_rgb` (0xRRGGBB) at most `0xFFFFFF`. A camera `path` becomes an animator on its entity with one clip, default index zero and loop wrap; its `node` selects the tracks at bake time, and they bind to that entity through `ANIM_TARGET_SELF` |
| Light | `kind` is `SCENE_LIGHT_KIND_DIRECTIONAL`; colour in 0..1; intensity finite and at least 0. The light shines along `SCENE_LIGHT_AXIS` (+Y) turned by its entity's rotation; at run time it lights skinned renderers only, baked renderers carry their light. Sky and camera regions are bake inputs and are not stored |
| Animator | `first_clip + clip_count` within the clip count; `default_clip` is below `clip_count` or `SCENE_ASSET_DEFAULT_CLIP_NONE` (no playback); `wrap` is an `ANIM_CLAMP` or `ANIM_LOOP` code from [`anim/anim_track.h`](../../../launcher/main/anim/anim_track.h) |
| Settings | ambient colour in 0..1; `ambient_intensity` and `tonemap_white` finite and at least 0; absent ambient has zero intensity. Applied before lights attach; `tonemap_white` 0 is `SCENE_TONEMAP_WHITE_NONE`, the identity tone map |
| Order | header, then parts in header order; every `0` above and every gap is zero |
| Errors | `ASSET_ERR_VERSION` for another version, v1 included, reported before its bounds; `ASSET_ERR_BOUNDS` for a part off its alignment or outside the entry; `ASSET_ERR_FORMAT` for the rest |
