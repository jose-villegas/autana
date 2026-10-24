# Baked Mesh

The mesh a bake writes and a renderer reads: its clusters and tree, the
`LMSH` entry layout, and the meshlets the clusters are. How a mesh is made is
[Mesh-Import.md](Mesh-Import.md); drawing it is [Mesh-Rendering.md](Mesh-Rendering.md).

## Clusters and the tree

Triangles group into **clusters**, the leaves of a tree rooted at `nodes[0]`:
one box test culls a subtree. Positions are `int16`, `position_scale` per unit.

![A baked mesh's clusters, then the tree level by level with one root-to-leaf path bold](../images/render/baked-mesh-tree.gif)

*Clusters, then each level's boxes with one root-to-leaf path bold.*

![The first leaves' clusters as runs of the triangle and vertex arrays](../images/render/baked-mesh-ranges.png)

*A cluster owns a run of each array and indexes only its own vertices.*

A vertex carries one sRGB colour, baked light times albedo. With
`shading = { flat = ... }` each triangle instead has one RGB565 colour
(`light.face_colours()`) averaged over the points `shading.flat` sets, and
vertices weld by position alone.

## LMSH

A baked mesh is an entry of type `LMSH` in the [asset pack](../assets/README.md).
`r3d_lit_mesh_from_asset()` fills an `r3d_lit_mesh_t` with pointers into the
entry, once, and nothing is allocated or copied; the rasterizer reads that
struct.

| Format | Version | Writer (Python) | Reader (C) |
|---|---|---|---|
| LMSH | none: the header has no version | `r3d/lit_mesh.py`, layout in `r3d/mesh_asset.py` | `render/r3d_lit_mesh.c` |

Little-endian; an array's offset counts from the entry's first byte.

```mermaid
---
config:
  packet:
    bitsPerRow: 12
    bitWidth: 60
---
packet-beta
title Header, 44 bytes (11 words) at 0, one cell a byte
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
```

```mermaid
---
config:
  packet:
    bitsPerRow: 12
    bitWidth: 60
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
  packet:
    bitsPerRow: 16
    bitWidth: 60
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
  packet:
    bitsPerRow: 6
    bitWidth: 84
---
packet-beta
title Content: face colour, 2 bytes a triangle from face_colours_off
0-1: "uint16 panel RGB565"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 44 bytes |
| Content: positions | `positions_off` | 6 bytes a vertex |
| Content: colours | `colours_off`, or 0 for a flat mesh | 3 bytes a vertex |
| Content: triangles | `triangles_off` | 6 bytes each |
| Cluster registry | `clusters_off` | 22 bytes a cluster, the layout of `r3d_lit_cluster_t` |
| Node registry | `nodes_off` | 16 bytes a node, the layout of `r3d_lit_node_t`; `nodes[0]` is the root |
| Content: face colours | `face_colours_off`, or 0 for a vertex-coloured mesh | 2 bytes a triangle |

| Rule | Detail |
|---|---|
| Owners | the header's word order is the `enum` in `r3d_lit_mesh.c`; `VERTEX_LIMIT` (65535) there caps the four counts (`ASSET_ERR_BOUNDS`); `r3d_lit_cluster_t` and `r3d_lit_node_t` are `_Static_assert`ed to 22 and 16 bytes with the field offsets above, and `lit_mesh.py`'s `CLUSTER` and `NODE` write the same bytes |
| Arrays | each offset a multiple of 4 and at least 44, the array inside the entry; the writer pads each array to 4 bytes and sets the offset of an empty one to 0 (`ASSET_ERR_BOUNDS`, also for a header shorter than 44 bytes); only colours and face colours may be 0 |
| Colour source | exactly one of colours and face colours, never both (`ASSET_ERR_FORMAT`) |
| Scale | `position_scale` is ticks per model unit, in 1..`INT_MAX` (`ASSET_ERR_FORMAT`); positions are int16 ticks |
| Counts | at least one cluster and one node (`ASSET_ERR_FORMAT`) |
| Clusters | vertex ranges tile every vertex once and in order: the first starts at 0, each starts at the previous end, the last ends at `vertex_count` (`ASSET_ERR_FORMAT`); each cluster's vertex and triangle ranges lie inside the mesh and its triangles index only its own vertices (`ASSET_ERR_BOUNDS`); the writer also tiles the triangle ranges in order |
| Nodes | a leaf's children are clusters, an inner node's are nodes, `first + count` inside that array; an inner node's children come after it, so a walk down the tree ends (`ASSET_ERR_BOUNDS`); the writer limits `count` to `MAX_NODE_CHILDREN` and hangs every cluster off exactly one leaf |
| Order | header, then the arrays in the order of the Part table, each padded to 4; padding zero |

## Meshlets

Clusters are meshoptimizer's **meshlets**<sup>[[11]](../Citations.md#11)</sup>:
at most `geometry.meshlet_triangles` (4 to 256, default 32) of one sidedness.
The octree is built over their centres.

![The clusterizer cutting a small flat patch into meshlets, one per step](../images/render/meshlet-build.gif)

*The importer's clusterizer call on a flat patch, limit 16: whole meshlets in
emitted order, since meshoptimizer has no per-triangle hook.*

![Clusters painted by the meshlets debug view](../images/render/sponza-meshlets.png)

![Clusters and submitted triangles against the meshlet size](../images/render/meshlets/meshlet-size-trade.png)

*Measured rows: the [size table](Render-Pipeline.md#meshlets).*

Meshlets share vertices, so a frame transforms fewer than with octree-cut
leaves. The draw order they set decides depth ties, a fraction of a percent of
pixels. Levels of detail on them: [plans/Cluster-LOD.md](../plans/Cluster-LOD.md).
