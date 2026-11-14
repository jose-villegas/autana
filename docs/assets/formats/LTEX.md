# LTEX v1 material texture

A material's albedo pattern as a tiled 4-bit indexed texture, which
[`render/texture/`](../../../launcher/main/render/texture/r3d_texture.h)
multiplies by a mesh's baked vertex light. Layout defines:
[`render/texture/r3d_texture.h`](../../../launcher/main/render/texture/r3d_texture.h).

The palette holds the pattern divided by its mean on screen, so a vertex
colour keeps its meaning (the surface's mean lit colour) and the texture only
modulates it. A mesh names its textures through its [LUVS](LUVS.md) entry.

A MASK1 texture cuts a triangle instead of patterning it, as a foliage card's
leaves: its texel 0 leaves the pixel as it was, and texel 1 draws palette
colour 1.

![Bistro's street on host: the source render, the vertex colours alone, and the cobblestones and beige brick textured, with each one's CIE76 error against the source](../../images/render/bistro-street-textured.png)

![The same frame's squares that differ most, zoomed: source, vertex colours and textured](../../images/render/bistro-street-textured.crops.png)

CIE76 is the distance between two colours in CIELAB; the error panels' scale
runs from black (matches the source) through red to yellow.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #f4a885 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 10
    bitWidth: 72
---
packet-beta
title Header, 20 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "format = 1"
4: "size_log2"
5: "levels"
6-8: "tint"
9-11: "reserved"
12-15: "palette_off"
16-19: "texels_off"
```

| Part | At | Size |
|---|---|---|
| Version | 0 | u16 |
| Format | 2 | u16, 1: INDEX4, 2: MASK1 |
| Side | 4 | u8, level 0 is `1 << size_log2` texels square |
| Levels | 5 | u8, level 0 and each mip, each half the last |
| Tint | 6 | 3 × u8, r, g, b: the hue the colormap lights the pattern with, the mean vertex colour it is drawn on |
| Reserved | 9 | 3 bytes, zero |
| Palette | `palette_off` | INDEX4: 16, MASK1: 2 × {u8 r, g, b, 0}, 128 = the pattern's mean |
| Texels | `texels_off` | each level's `side² / 2` (INDEX4) or `side² / 8` (MASK1) bytes, level 0 first |

| Rule | Detail |
|---|---|
| Owners | Layout constants in [`render/texture/r3d_texture.h`](../../../launcher/main/render/texture/r3d_texture.h); Python reads them with `read_define`. INDEX4 writer: [`r3d/texture/texture_asset.py`](../../../launcher/tools/r3d/texture/texture_asset.py); C opener: `r3d_texture_from_asset()` |
| Texels | Row-major. INDEX4: two texels a byte, the low nibble the even column. MASK1: eight a byte, the low bit the first; 0 draws nothing. A sampler wraps u and v by the level's side |
| Sides | `size_log2` up to 8; the last level is at least 2 texels a side for INDEX4, 4 for MASK1, so every level is whole bytes |
| Palette | A channel of 128 is the pattern's mean; up to 255, twice it, as the PS1's modulate. The device builds a colormap of `R3D_TEXTURE_LIGHT_LEVELS` light levels from the palette and the tint at open |
| Refusals | Versions other than 1 return `ASSET_ERR_VERSION`; another format, side or level count, or nonzero reserved bytes `ASSET_ERR_FORMAT`; a palette or level outside the entry `ASSET_ERR_BOUNDS` |
