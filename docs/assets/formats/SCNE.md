# SCNE v7 scene

One scene: its entities, their transforms and parents, the components on them, and the copies a renderer draws from one table ([Batches](#batches)) ([Scene-Files.md](../../render/scene/Scene-Files.md) is the authored file, [Scene-Manager.md](../../render/scene/Scene-Manager.md) the runtime). Layout defines: [`scene/scene_format.h`](../../../launcher/main/scene/scene_format.h); row structs in [`scene/scene_asset.h`](../../../launcher/main/scene/scene_asset.h).

`build_pack.py` bakes `NAME.scene.toml` into entry `NAME` of its own pack `NAME`, beside every mesh, skin, skeleton and clip it names; a scene and its clip cannot share a stem ([Building-Packs.md](../Building-Packs.md#packs)). `scene_load()` opens it and each entry it names, and fails naming whichever is missing or malformed.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(8) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(5) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(6) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(7) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(8) { fill: #c7aaee !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(4) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(4) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(4) rect:nth-of-type(5) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(6) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(4) { fill: #f4a885 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(6) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(6) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(6) rect:nth-of-type(4) { fill: #f4a885 !important; } g:nth-of-type(7) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(7) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(7) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(7) rect:nth-of-type(4) { fill: #f4a885 !important; } g:nth-of-type(8) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(8) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(8) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(8) rect:nth-of-type(4) { fill: #f4a885 !important; } g:nth-of-type(9) rect:nth-of-type(1) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 64
---
packet-beta
title Header, 116 bytes at 0, one cell a byte
0-1: "version = 7"
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
22-23: "batches"
24-25: "chunks"
26-27: "places"
28-29: "turns"
30-31: "timeline"
32-33: "pos count"
34-35: "aim count"
36-37: "sources"
38-39: "hinges"
40-43: "names_off"
44-47: "transforms_off"
48-51: "renderers_off"
52-55: "cameras_off"
56-59: "lights_off"
60-63: "skins_off"
64-67: "animators_off"
68-71: "clips_off"
72-75: "settings_off"
76-79: "parents_off"
80-83: "batches_off"
84-87: "chunks_off"
88-91: "places_off"
92-95: "turns_off"
96-99: "timeline_off"
100-103: "position_off"
104-107: "aim_off"
108-111: "sources_off"
112-115: "hinges_off"
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

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 44
---
packet-beta
title Content: batch, 12 bytes from batches_off
0-1: "renderer"
2-3: "copy_count"
4-7: "first_copy"
8-11: "first_chunk"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Content: chunk, 16 bytes from chunks_off
0-11: "f32 centre[3]"
12-15: "f32 reach"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 44
---
packet-beta
title Content: place, 8 bytes a copy from places_off
0-5: "i16 offset[3]"
6-7: "colour_set"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 44
---
packet-beta
title Content: turn, 8 bytes a copy from turns_off
0-5: "i16 rotation[3]"
6-7: "largest"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 54
---
packet-beta
title Content: timeline, 36 bytes from timeline_off, at most one
0-31: "char clip[32], NUL padded"
32: "wrap"
33-35: "0"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 80
---
packet-beta
title Position constraint, 24 bytes from position_off
0-1: "entity"
2: "bind mode"
3: "src count"
4-5: "source_first"
6-7: "0"
8-11: "f32 weight"
12-23: "f32 translation_offset[3]"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 60
---
packet-beta
title Aim constraint, 64 bytes from aim_off
0-1: "entity"
2: "up type"
3: "src count"
4-5: "source_first"
6-7: "world_up_object"
8-11: "f32 weight"
12-23: "f32 aim_vector[3]"
24-35: "f32 up_vector[3]"
36-47: "f32 world_up_vector[3]"
48-63: "f32 rotation_offset[4]"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 80
---
packet-beta
title Constraint source, 8 bytes from sources_off
0-3: "u32 target (name_hash)"
4-7: "f32 weight"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 54
---
packet-beta
title Content: hinges, 28 bytes each from hinges_off
0-1: "entity"
2-3: "0"
4-15: "f32 axis[3]"
16-19: "f32 min"
20-23: "f32 max"
24-27: "f32 angle"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 116 bytes. A u16 count and a u32 offset per part, in `scene_asset_part_t` order |
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
| Content: batches | `batches_off` | 12 bytes each, one for each renderer that draws copies |
| Content: chunks | `chunks_off` | 16 bytes each, each batch's chunks in turn |
| Content: places | `places_off` | 8 bytes a copy, each batch's copies in turn |
| Content: turns | `turns_off` | 8 bytes a copy, in the places' order; the count equals the places' |
| Content: timeline | `timeline_off` | 36 bytes, at most one row |
| Content: position constraints | `position_off` | 24 bytes each |
| Content: aim constraints | `aim_off` | 64 bytes each |
| Constraint sources | `sources_off` | 8 bytes each, shared by both kinds |
| Content: hinges | `hinges_off` | 28 bytes each |

| Rule | Detail |
|---|---|
| Owners | `SCENE_ASSET_VERSION`, `_HEADER_SIZE` (116), `_AT_COUNTS`, `_AT_OFFSETS`, `SCENE_ASSET_DEFAULT_CLIP_NONE` (0xFFFF), `SCENE_ASSET_PARENT_ROOT` (0xFFFF), `SCENE_ASSET_COLOUR_SET_OWN` (0xFFFF), `SCENE_LIGHT_KIND_DIRECTIONAL` (1), `SCENE_ASSET_NAME_SIZE` (36), `_NAME_AT_HASH` (0), `_NAME_AT_TEXT` (4) in `scene_format.h`; row structs in `scene_asset.h`, checked against the sizes above by `_Static_assert` |
| Layout | every part offset is a multiple of 4, at least 116. Parts end inside the entry and occupy disjoint ranges; the writer lays parts out in header order, each padded to 4 bytes |
| Counts | each at most 65535, entities at most 65534; names equals transforms and parents; turns equals places; settings is exactly 1; timeline is at most 1 |
| Names | letters, digits and `_`, starting with a letter or `_`, at most 31 bytes; `name_hash` is `name_hash(name)` ([hash package](../../../launcher/packages/hash/README.md)) and never `NAME_HASH_NONE`, and finds compare it alone. The bake refuses a name that hashes to `NAME_HASH_NONE` and two names in one scene that share a hash, naming both |
| Transforms | in the parent's frame; scale positive. `scene_load()` refuses a shear or a reflection (determinant −1) and names its entity |
| Parents | `parent` is `SCENE_ASSET_PARENT_ROOT` or below its own entity, so one forward pass meets every parent before its children. A camera, light or skinned renderer is a root. An entity with children has a uniform scale, the same on every axis, and `scene_load()` refuses one that does not, naming it; `scene_asset.encode()` folds a parent's uneven scale into its children, refusing a parent that draws or a turned child the fold would shear. A mesh bundle's copies of one shape are one batch, on a child of the renderer that carries them ([MCOP](MCOP.md)). Transform and enabled state carry to descendants ([Scene-Manager.md](../../render/scene/Scene-Manager.md#hierarchy)) |
| Entities | the scene's objects in file order; every `entity` is below the names' count; at most one light, one skinned renderer, one animator, one position constraint, one aim constraint and one hinge per entity |
| Colour sets | own colours (`SCENE_ASSET_COLOUR_SET_OWN`) need no LCOL, checked at pack time, and authored scenes write own colours; [Selection and ownership](LCOL.md) |
| Text | `name`, `mesh_id`, `skin_id`, `skeleton_id` and clip ids are nonempty, NUL terminated, zero after the NUL |
| Camera | `half_fov_short_tan` and `near_z` finite and positive; `clear_rgb` (0xRRGGBB) at most `0xFFFFFF`. A camera `path` becomes an animator on its entity with one clip, default index zero and loop wrap; its `node` selects the tracks at bake time, and they bind to that entity through `ANIM_TARGET_SELF` |
| Light | `kind` is `SCENE_LIGHT_KIND_DIRECTIONAL`; colour in 0..1; intensity finite and at least 0. The light shines along `SCENE_LIGHT_AXIS` (+Y) turned by its entity's rotation; at run time it lights skinned renderers only, baked renderers carry their light. Sky and camera regions are bake inputs and are not stored |
| Animator | `first_clip + clip_count` within the clip count; `default_clip` is below `clip_count` or `SCENE_ASSET_DEFAULT_CLIP_NONE` (no playback); `wrap` is an `ANIM_CLAMP` or `ANIM_LOOP` code from [`anim/anim_track.h`](../../../launcher/main/anim/anim_track.h) |
| Timeline | The clip names a TRCK in the same pack with root `ANIM_ROOT_SCENE`. Wrap is `ANIM_CLAMP` or `ANIM_LOOP`. |
| Constraints | Floats are finite and weights are in 0..1. Each `source_count` is 1 through `SCENE_CONSTRAINT_SOURCES_MAX` and `source_first + source_count` is at most the source table count. |
| Position constraint | `binding_mode` is `world_space` (0), `lock_to_target` (1) or `lock_to_target_with_world_up` (2). Padding is zero. |
| Aim constraint | `world_up_type` is `scene_up` (0), `object_up` (1), `object_rotation_up` (2), `vector` (3) or `none` (4). `world_up_object` is an entity row for types 1 and 2 and `SCENE_ASSET_WORLD_UP_OBJECT_NONE` (0xFFFF) otherwise. Aim and up vectors are nonzero. The tool bakes Euler degrees to an x, y, z, w quaternion. |
| Constraint source | `target` is the hash of a named scene entity. `weight` is finite and in 0..1. |
| Hinges | Floats are finite and angles are in radians with `min <= max`. The axis has unit length within the squared-length tolerance `SCENE_ASSET_HINGE_AXIS_TOLERANCE`. |
| Unsupported rows | `scene_load()` rejects a scene with a position or aim constraint as unsupported and names `constraint`. It names `hinge` for a hinge. |
| Settings | ambient colour in 0..1; `ambient_intensity` and `tonemap_white` finite and at least 0; absent ambient has zero intensity. Applied before lights attach; `tonemap_white` 0 is `SCENE_TONEMAP_WHITE_NONE`, the identity tone map |
| Batches | see [Batches](#batches) |
| Order | header, then parts in header order; every `0` above and every gap is zero |
| Errors | `ASSET_ERR_VERSION` for any other version, reported before its bounds; `ASSET_ERR_BOUNDS` for a part off its alignment or outside the entry; `ASSET_ERR_FORMAT` for the rest |

## Batches

A batch draws copies of one mesh from packed rows: one renderer and one entity, not one each. `render/batch/r3d_batch.h` owns the constants and decodes the rows; the cull tests each chunk's sphere, then each copy's, and draws the copies it keeps.

```mermaid
flowchart LR
    B["batch: renderer, copies"] --> C["chunk: centre, reach"]
    C --> P["place: offset, colour set"]
    P --> T["turn: rotation, read for kept copies"]
```

| Rule | Detail |
|---|---|
| Owners | `R3D_BATCH_CHUNK_COPIES` (16), `R3D_COPY_STEPS` (32767), `R3D_COPY_TURN_RANGE` (1/√2) in [`render/batch/r3d_batch.h`](../../../launcher/main/render/batch/r3d_batch.h) |
| Renderer | `renderer` names a renderers row, at most one batch each; that row's `colour_set` is 0, and its entity's transform is the batch's frame |
| Cover | batches take the places, turns and chunks in turn: `first_copy` is the copies before it, `first_chunk` the chunks before it, and together they cover every row; `copy_count` is at least 1 |
| Chunks | a batch's copy *k* is in its chunk *k* / 16; `centre` is finite, `reach` at least 0 and no copy's position farther from `centre`. The writer orders a batch's copies by the Morton code of their positions, so a chunk's copies are near each other |
| Position | `centre + offset * reach / R3D_COPY_STEPS`, the batch's frame |
| Turn | the three smaller components of the copy's unit quaternion, each in `R3D_COPY_STEPS` steps of `R3D_COPY_TURN_RANGE`, and `largest` (0 to 3: x, y, z, w) the one left out, never negative, rebuilt from the others. A copy turns and moves only: the writer refuses one that scales, shears or reflects |
| Colour set | each copy's LCOL set, below the mesh's set count; `scene_load()` opens the highest. The sets are resident together, set *s* at *s* times a set's length |
