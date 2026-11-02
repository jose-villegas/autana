# LMSH lit mesh

The baked mesh a renderer draws, its clusters and tree explained in [Baked-Mesh.md](../../render/offline/Baked-Mesh.md). The tag and the `r3d_lit_cluster_t` and `r3d_lit_node_t` records are in [`render/r3d_lit_mesh.h`](../../../launcher/main/render/r3d_lit_mesh.h).

A baked mesh is an entry of type `LMSH` in the [asset pack](../README.md).
`r3d_lit_mesh_from_asset()` fills an `r3d_lit_mesh_t` with pointers into the
entry, once, and nothing is allocated or copied; the rasterizer reads that
struct.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(3) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(5) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(6) rect:nth-of-type(2) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 60
---
packet-beta
title Header, 56 bytes (14 words) at 0, one cell a byte
0-3: "vertex_count"
4-7: "triangle_count"
8-11: "cluster_count"
12-15: "node_count"
16-19: "position_scale"
20-23: "positions_off"
24-27: "colours_off"
28-31: "triangles_off"
32-35: "clusters_off"
36-39: "nodes_off"
40-43: "face_colours_off"
44-47: "lod_group_count"
48-51: "cluster_lod_off"
52-55: "lod_groups_off"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(3) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Cluster registry, one 22-byte row a cluster from clusters_off
0-1: "vertex_first"
2-3: "vertex_count"
4-5: "triangle_first"
6-7: "triangle_count"
8-13: "int16 lo[3]"
14-19: "int16 hi[3]"
20: "double_sided"
21: "0"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(4) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 104
---
packet-beta
title Node registry, one 16-byte row a node from nodes_off, root first
0-5: "int16 lo[3]"
6-11: "int16 hi[3]"
12-13: "first"
14: "count"
15: "leaf"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 84
---
packet-beta
title Content: position, 6 bytes a vertex from positions_off
0-1: "int16 x"
2-3: "int16 y"
4-5: "int16 z"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 84
---
packet-beta
title Content: triangle, 6 bytes from triangles_off
0-1: "uint16 a"
2-3: "uint16 b"
4-5: "uint16 c"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 84
---
packet-beta
title Content: colour, 3 bytes a vertex from colours_off
0: "r"
1: "g"
2: "b"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 84
---
packet-beta
title Content: face colour, 2 bytes a triangle from face_colours_off
0-1: "uint16 panel RGB565"
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
title Levels: cluster LOD, 4 bytes a cluster from cluster_lod_off
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
title Levels: LOD group, 10 bytes a group from lod_groups_off
0-5: "int16 centre[3]"
6-7: "int16 radius"
8-9: "uint16 error"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 56 bytes |
| Content: positions | `positions_off` | 6 bytes a vertex |
| Content: colours | `colours_off`, or 0 when absent | 3 bytes a vertex |
| Content: triangles | `triangles_off` | 6 bytes each |
| Cluster registry | `clusters_off` | 22 bytes a cluster, the layout of `r3d_lit_cluster_t` |
| Node registry | `nodes_off` | 16 bytes a node, the layout of `r3d_lit_node_t`; `nodes[0]` is the root |
| Content: face colours | `face_colours_off`, or 0 when absent | 2 bytes a triangle |
| Levels: cluster LOD | `cluster_lod_off`, or 0 without levels | 4 bytes a cluster, the layout of `r3d_lit_cluster_lod_t` |
| Levels: LOD groups | `lod_groups_off`, or 0 without levels | 10 bytes a group, the layout of `r3d_lit_lod_group_t` |

| Rule | Detail |
|---|---|
| Owners | the header's word order is the `enum` in `r3d_lit_mesh.c`; `VERTEX_LIMIT` (65535) there caps the four counts (`ASSET_ERR_BOUNDS`); `r3d_lit_cluster_t` and `r3d_lit_node_t` are `_Static_assert`ed to 22 and 16 bytes, `r3d_lit_cluster_lod_t` and `r3d_lit_lod_group_t` to 4 and 10, with the field offsets above, and `mesh_asset.py`'s `CLUSTER`, `NODE`, `CLUSTER_LOD` and `LOD_GROUP` write the same bytes |
| Arrays | each offset a multiple of 4 and at least 44, the array inside the entry; the writer pads each array to 4 bytes and sets the offset of an empty one to 0 (`ASSET_ERR_BOUNDS`, also for a header shorter than 56 bytes); only colours, face colours and the two level arrays may be 0 |
| Colour source | At most one of colours and face colours (`ASSET_ERR_FORMAT` for both) |
| Scale | `position_scale` is ticks per model unit, in 1..`INT_MAX` (`ASSET_ERR_FORMAT`); positions are int16 ticks |
| Counts | at least one cluster and one node (`ASSET_ERR_FORMAT`) |
| Clusters | vertex ranges tile every vertex once and in order: the first starts at 0, each starts at the previous end, the last ends at `vertex_count` (`ASSET_ERR_FORMAT`); each cluster's vertex and triangle ranges lie inside the mesh and its triangles index only its own vertices (`ASSET_ERR_BOUNDS`); the writer also tiles the triangle ranges in order |
| Nodes | a leaf's children are clusters, an inner node's are nodes, `first + count` inside that array; an inner node's children come after it, so a walk down the tree ends, and number at most `R3D_LIT_NODE_CHILDREN_MAX` (8), the octree's split (`ASSET_ERR_BOUNDS`); the writer limits `count` to `MAX_NODE_CHILDREN` and hangs every cluster off exactly one leaf |
| Levels | whole or absent: `lod_group_count`, `cluster_lod_off` and `lod_groups_off` all 0 or all set (`ASSET_ERR_FORMAT`); `lod_group_count` below `R3D_LOD_NONE` (65535, `ASSET_ERR_BOUNDS`); written only when the bake's `[geometry] lod` is on, and level 0 is then clusterlod's finest clusters |
| Group ids | `own` is the group simplified into the cluster, `R3D_LOD_NONE` on a finest cluster; `parent` the group it belongs to, `R3D_LOD_NONE` when that group is never drawn coarse; each names a group or none (`ASSET_ERR_FORMAT`) |
| Group bounds | centre, radius and error in position ticks; radius at least 0; where a cluster has both ids, its own group sits inside its parent with `R3D_LOD_ENCLOSE_MARGIN` (1) ticks to spare and has no larger error (`ASSET_ERR_FORMAT`), so a parent never projects smaller and the pick's cut cannot crack |
| Order | header, then the arrays in the order of the Part table, each padded to 4; padding zero |
