# LUVS v1 mesh texturing

Which clusters of an LMSH draw textured, with which [LTEX](LTEX.md) material,
and each vertex's UV. Layout defines:
[`render/texture/r3d_texture.h`](../../../launcher/main/render/texture/r3d_texture.h).

The entry is the mesh's sibling `<mesh_id>.uvs`; its material k is the LTEX
sibling `<mesh_id>.t<k>`. A mesh without one draws plain. The texture bake
(`bake.py`'s `texture` kind) writes both from the packed mesh and its source,
for the renderers a scene's `<scene>.textures.toml` names.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 60
---
packet-beta
title Header, 24 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "material_count"
4-7: "vertex_count"
8-11: "cluster_count"
12-15: "uvs_off"
16-19: "origins_off"
20-23: "clusters_off"
```

| Part | At | Size |
|---|---|---|
| Version | 0 | u16 |
| Materials | 2 | u16, 1 to 255 |
| Vertices | 4 | u32, the mesh's |
| Clusters | 8 | u32, the mesh's |
| UVs | `uvs_off` | `vertex_count` × {i16 u, v}, 1/16 texel of level 0 from the vertex's cluster's origin |
| Origins | `origins_off` | `cluster_count` × {i16 u, v}, whole texture sides |
| Cluster materials | `clusters_off` | `cluster_count` × u8, a material index or 255 for none |

| Rule | Detail |
|---|---|
| Owners | Layout constants in `render/texture/r3d_texture.h`; Python reads them with `read_define`. Writer: `r3d/texture/uv_transfer.py`; C opener: `r3d_texture_texturing_from_asset()` |
| UVs | A cluster's vertices all take its material's UVs: a vertex's texel is `origin × side + uv / 16`; origins keep a wall many repeats long within int16 |
| Clusters | A cluster of material 255 draws with its vertex colours alone |
| Layout | Little-endian; the UV and origin sections 2-byte aligned; no trailing bytes |
| Refusals | Versions other than 1 return `ASSET_ERR_VERSION`; counts that differ from the mesh's, no materials, or a material index past the count `ASSET_ERR_FORMAT`; a section outside the entry or misaligned `ASSET_ERR_BOUNDS`; a missing material `ASSET_ERR_NOT_FOUND` |
