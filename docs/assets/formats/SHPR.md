# SHPR v1 probe grid

Incident radiance in a regular grid of real L1 spherical harmonics.
The [probe baker](../../../launcher/tools/r3d/sh_probe_bake.py) owns the encoding;
[baking](../../render/offline/SH-Probes.md) describes the sampling.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Header, 40 bytes, one cell a byte
0-1: "version = 1"
2-3: "reserved"
4-15: "origin: f32 xyz"
16-27: "spacing: f32 xyz"
28-31: "scale: f32"
32-37: "dims: u16 xyz"
38-39: "0 (reserved)"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 120
---
packet-beta
title Probe, 24 bytes, one cell a byte
0-5: "Y00: i16 RGB"
6-11: "Y1(-1): i16 RGB"
12-17: "Y1(0): i16 RGB"
18-23: "Y1(1): i16 RGB"
```

| Field | Offset | Encoding |
|---|---|---|
| Version | 0 | u16, `VERSION` |
| Flags | 2 | u16, reserved, must be zero |
| Origin | 4 | Three f32, first cell centre in source coordinates |
| Spacing | 16 | Three positive f32, cell step in source units |
| Scale | 28 | Positive f32; coefficient = signed integer × scale |
| Dimensions | 32 | Three positive u16, x/y/z cell counts |
| Padding | 38 | Two zero bytes |
| Probes | 40 | `dims.x * dims.y * dims.z` records, each 12 i16, x fastest |

All multibyte fields are little-endian and aligned to their element, without a
magic prefix. The pack entry type is `SHPR`. The payload ends after the last
probe. Origin, spacing, scale and coefficients must be finite.

V1 includes direct lighting in the incident-radiance coefficients.

The coefficient order is constant, +y, +z, +x, each with consecutive RGB.
`Y00 = 0.5 sqrt(1/pi)` and `Y1 = 0.5 sqrt(3/pi)` follow
Sloan 2008<sup>[[129]](../../Citations.md#sloan-2008)</sup>.
Scale covers the largest absolute coefficient without clipping; an all-zero
grid uses scale 1.

[`sh_probes.shpr`](../../../launcher/tools/tests/data/sh_probes.shpr) is the
cross-language golden fixture: origin `(1,2,3)`, spacing `(4,5,6)`, dimensions
`(2,1,1)`, scale `0.25`, stored integers `-12..11` in wire order.
