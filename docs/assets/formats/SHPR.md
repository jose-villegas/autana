# SHPR v1 probe grid

Incident radiance in a regular grid of real L1 spherical harmonics.
The [probe baker](../../../launcher/tools/r3d/sh_probe_bake.py) owns the encoding;
[baking](../../render/offline/SH-Probes.md) describes the sampling.

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Header, 36 bytes, one cell a byte
0: "version = 1"
1: "flags"
2-13: "origin: f32 xyz"
14-25: "spacing: f32 xyz"
26-31: "dims: u16 xyz"
32-35: "scale: f32"
```

```mermaid
---
config:
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
| Version | 0 | u8, `VERSION` |
| Flags | 1 | u8; bit 0 `DIRECT_INCLUDED`; bits 1..7 reserved, zero |
| Origin | 2 | Three f32, first cell centre in source coordinates |
| Spacing | 14 | Three positive f32, cell step in source units |
| Dimensions | 26 | Three positive u16, x/y/z cell counts |
| Scale | 32 | Positive f32; coefficient = signed integer × scale |
| Probes | 36 | `dims.x * dims.y * dims.z` records, each 12 i16, x fastest |

All multibyte fields are little-endian, tightly packed, without padding or a
magic prefix. The pack entry type is `SHPR`. The payload ends after the last
probe. Origin, spacing, scale and coefficients must be finite.

The coefficient order is constant, +y, +z, +x, each with consecutive RGB.
`Y00 = 0.5 sqrt(1/pi)` and `Y1 = 0.5 sqrt(3/pi)` follow
Sloan 2008<sup>[[129]](../../Citations.md#sloan-2008)</sup>.
Scale covers the largest absolute coefficient without clipping; an all-zero
grid uses scale 1.

[`sh_probes.shpr`](../../../launcher/tools/tests/data/sh_probes.shpr) is the
cross-language golden fixture: origin `(1,2,3)`, spacing `(4,5,6)`, dimensions
`(2,1,1)`, scale `0.25`, stored integers `-12..11` in wire order.
