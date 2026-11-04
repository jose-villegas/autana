# LLOD v1 mesh levels

Levels for a sibling [LMSH](LMSH.md), named `<mesh_id>.lod`.
Layout defines: [`render/llod_format.h`](../../../launcher/main/render/llod_format.h).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Header, 8 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "group_count"
4-7: "cluster_count"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 4
    bitWidth: 120
---
packet-beta
title Levels: cluster LOD, 4 bytes a cluster from byte 8
0-1: "uint16 own"
2-3: "uint16 parent"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Levels: LOD group, 10 bytes a group after cluster records
0-5: "int16 centre[3]"
6-7: "int16 radius"
8-9: "uint16 error"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | `LLOD_HEADER_SIZE` bytes |
| Cluster records | `LLOD_HEADER_SIZE` | `cluster_count × sizeof(r3d_lit_cluster_lod_t)` |
| Group records | after cluster records | `group_count × sizeof(r3d_lit_lod_group_t)` |

| Rule | Detail |
|---|---|
| Owners | `render/llod_format.h` owns the header and suffix; `r3d_lit_mesh.h` owns both record layouts; `r3d/lod_asset.py` reads the defines and writes the entry |
| Version | `LLOD_VERSION`; other versions return `ASSET_ERR_VERSION` |
| Counts | Cluster count equals the mesh's; at least one group (`ASSET_ERR_FORMAT`), fewer than `R3D_LOD_NONE` groups (`ASSET_ERR_BOUNDS`) |
| Size | Exactly header, cluster records, group records, with no trailing bytes (`ASSET_ERR_BOUNDS`); little-endian, 4-byte aligned entry |
| Group ids | `own` is the group simplified into the cluster, `R3D_LOD_NONE` on a finest cluster; `parent` the group it belongs to, `R3D_LOD_NONE` when that group is never drawn coarse; each names a group or none (`ASSET_ERR_FORMAT`) |
| Group bounds | centre, radius and error in position ticks; radius at least 0; where a cluster has both ids, its own group sits inside its parent with `R3D_LOD_ENCLOSE_MARGIN` (1) ticks to spare and has no larger error (`ASSET_ERR_FORMAT`), so a parent never projects smaller and the pick's cut cannot crack |
| Opening | `r3d_lit_mesh_levels_from_asset()` attaches validated levels to an already loaded mesh; failure clears only its level pointers and count |
| Sibling | `r3d_lit_mesh_open()` resolves the sibling once; absent means finest only, any other failure clears the mesh and returns the error |

Level 0 is clusterlod's finest clusters. The import's `[geometry] lod` setting
controls whether the CPU bake builds a hierarchy.
