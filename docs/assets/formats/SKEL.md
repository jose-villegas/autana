# SKEL v1 skeleton

A skinned mesh's joint tree and rest pose ([Skeleton-and-Skin.md](../../render/animation/Skeleton-and-Skin.md)). Layout defines: [`anim/anim_skeleton_format.h`](../../../launcher/main/anim/anim_skeleton_format.h).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #f4a885 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Header, 16 bytes at 0, one cell a byte
0-1: "version = 1"
2: "joints"
3: "0"
4-7: "strings_off"
8-11: "strings_size"
12-15: "rest_off"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 4
    bitWidth: 120
---
packet-beta
title Joint registry, one 4-byte row a joint from byte 16
0-1: "path"
2: "parent"
3: "0"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 20
    bitWidth: 44
---
packet-beta
title Content: rest records, one 40-byte record a joint from rest_off
0-11: "f32 position[3]"
12-27: "f32 rotation[4], xyzw"
28-39: "f32 scale[3]"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 16 bytes |
| Joint registry | 16 | 4 bytes a joint; `path` is an offset within the strings |
| Content: rest records | `rest_off` | 40 bytes a joint |
| Content: strings | `strings_off` | UTF-8 joint paths, NUL terminated; ends at the entry end |

| Rule | Detail |
|---|---|
| Owners | `ANIM_SKELETON_VERSION`, `_HEADER_SIZE` (16), `_ROW_SIZE` (4), `_REST_WIDTH` (10 floats), `_JOINT_MAX` (254), `_ROOT` (255), `_STRING_MAX`, `_ALIGNMENT` (4) and the `_AT_*` offsets in `anim_skeleton_format.h` |
| Joints | 1..254 |
| Parents | `0xFF` is a root; any other parent is below its joint's index, so joint 0 is a root. Two roots under one armature node are allowed |
| Paths | from `gltf_read.skin_joint_paths`, starting at each root; nonempty, unique (checked by Python and C), each terminated within the table, which fits a u16 offset |
| Rest | finite; rotation with `abs(1 - sum(q*q))` at most `ANIM_SKELETON_UNIT_TOLERANCE` (1e-4, Python `UNIT_TOLERANCE`), boundary tests at the adjacent floats |
| Order | header, joint registry, rest records, strings; gaps and reserved bytes zero |
