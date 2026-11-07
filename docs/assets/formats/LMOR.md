# LMOR v1 vertex morph targets

Geomorph targets for a sibling [LMSH](LMSH.md) that has [LLOD](LLOD.md) levels, named `<mesh_id>.morph`.
Layout defines: [`render/lmor_format.h`](../../../launcher/main/render/lmor_format.h).

Each vertex of a cluster with a parent group stores where it lands on that
group's coarse surface. The transform blends it there as the group nears its
switch ([Cluster-LOD.md](../../plans/Cluster-LOD.md#the-blend)).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 60
---
packet-beta
title Header, 12 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "flags"
4-7: "cluster_count"
8-11: "vertex_count"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 4
    bitWidth: 120
---
packet-beta
title Shift, 1 byte a cluster from byte 12
0-0: "uint8 shift"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 4
    bitWidth: 120
---
packet-beta
title Delta, 3 bytes a vertex after the shifts; a colour target is 3 bytes a vertex after the deltas
0-0: "int8 x"
1-1: "int8 y"
2-2: "int8 z"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | `LMOR_HEADER_SIZE` bytes |
| Shifts | `LMOR_HEADER_SIZE` | `cluster_count` bytes, padded to 4 |
| Deltas | after the shifts | `vertex_count × LMOR_AXES` bytes, padded to 4 |
| Colour targets | after the deltas, with `LMOR_COLOURS` | `vertex_count × LMOR_AXES` bytes, padded to 4 |

| Rule | Detail |
|---|---|
| Owners | `render/lmor_format.h` owns the header, flags and suffix; `r3d/morph_asset.py` reads the defines and writes the entry |
| Version | `LMOR_VERSION`; other versions return `ASSET_ERR_VERSION` |
| Counts | Cluster and vertex counts equal the mesh's, and the mesh has levels (`ASSET_ERR_FORMAT`) |
| Size | Exactly the parts the flags name, with no trailing bytes (`ASSET_ERR_BOUNDS`) |
| Target | A vertex's target in ticks is its position plus `delta << shift`, its cluster's shift at most `LMOR_SHIFT_MAX` (`ASSET_ERR_FORMAT`). A delta is -127 to 127, so mirroring an axis still fits |
| Colours | `LMOR_COLOURS` only on a mesh with vertex colours (`ASSET_ERR_FORMAT`); a face-coloured or colourless mesh blends positions only |
| No parent | A cluster without a parent group holds deltas of 0, never read |
| Opening | `r3d_lit_mesh_morph_from_asset()` attaches validated targets; failure clears only the morph pointers. `r3d_lit_mesh_open()` resolves the sibling after the levels; absent means no blend |
