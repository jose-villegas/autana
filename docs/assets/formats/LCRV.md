# LCRV thin curves

Radius-bearing polylines beside an object's LMSH. The reader holds pointers into
the pack; positions and radii use the LMSH's position scale.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Header, 8 bytes, one cell a byte
0-1: "curve count"
2-3: "knot count"
4-7: "reserved"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Curve row, 16 bytes, one cell a byte
0-1: "first knot"
2-3: "knot count"
4-9: "box lo, xyz i16"
10-15: "box hi, xyz i16"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 10
    bitWidth: 80
---
packet-beta
title Knot row, 10 bytes, one cell a byte
0-5: "xyz i16"
6-7: "radius u16"
8-9: "RGB565"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 8 bytes |
| Curve rows | 8 | 16 bytes per curve |
| Knot rows | `8 + 16 * curve_count` | 10 bytes per knot; ends at entry end |

| Rule | Detail |
|---|---|
| Owners | `r3d/thin/curve_asset.py`; `render/curve/r3d_curve.h` and `r3d_lit_mesh.c` |
| Sibling | Entry type `LCRV`, named LMSH id plus `.crv`; optional |
| Byte order | Little-endian integers; colour stores panel byte-swapped RGB565 |
| Position | Signed ticks; model position is stored position divided by LMSH `position_scale` |
| Radius | Unsigned ticks at the same scale; nonnegative |
| Bounds | Signed ticks; includes every knot's radius, including quantisation rounding |
| Ranges | Curves own contiguous, disjoint knot ranges in row order; each has at least two knots |
| Counts | Both totals fit u16; empty sets have no rows |
| Reserved | Zero; no padding between records |
| Colour | Linear albedo times mean ring radiance, tone-mapped with the mesh's white, encoded to sRGB and packed |
