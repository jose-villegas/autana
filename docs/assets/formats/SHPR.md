# SHPR v1 probe grid

Incident radiance in a regular grid of real L1 spherical harmonics.
The [probe baker](../../../launcher/tools/r3d/sh_probe_asset.py) owns the encoding;
[baking](../../render/offline/SH-Probes.md) describes the sampling.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(2) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Header, 40 bytes, one cell a byte
0-1: "version = 1"
2-3: "flags: u16"
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
| Flags | 2 | u16; bit 0 is `SH_PROBES_FLAG_INSIDE_MASK`, other bits must be zero |
| Origin | 4 | Three f32, first cell centre in engine coordinates |
| Spacing | 16 | Three positive f32, cell step in engine units |
| Scale | 28 | Positive f32; coefficient = signed integer × scale |
| Dimensions | 32 | Three positive u16, x/y/z cell counts |
| Padding | 38 | Two zero bytes |
| Probes | 40 | `dims.x * dims.y * dims.z` records, each 12 i16, x fastest |
| Inside mask | After probes | When bit 0 is set, `ceil(n / 8)` bytes for `n` probes; bit `i % 8` of byte `i / 8` marks probe `i` inside geometry |

All multibyte fields are little-endian and aligned to their element, without a
magic prefix. The pack entry type is `SHPR`. The payload length is exactly
`40 + 24 * n`, plus `ceil(n / 8)` when the mask flag is set.
Origin, spacing, scale and coefficients must be finite.

The baker always writes the mask. Unused bits in its last byte are zero.
A payload without the flag has no inside probes.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(8) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Inside mask byte, one cell a bit, least significant first
0: "probe 8k"
1: "8k + 1"
2: "8k + 2"
3: "8k + 3"
4: "8k + 4"
5: "8k + 5"
6: "8k + 6"
7: "8k + 7"
```

V1 includes direct lighting in the incident-radiance coefficients.

The coefficient order is constant, +y, +z, +x, each with consecutive RGB.
$`Y_{00} = \tfrac12\sqrt{1/\pi}`$ and $`Y_1 = \tfrac12\sqrt{3/\pi}`$ follow
Sloan 2008<sup>[[129]](../../Citations.md#sloan-2008)</sup>.
Scale covers the largest absolute coefficient without clipping; an all-zero
grid uses scale 1.

[`sh_probes.shpr`](../../../launcher/tools/tests/data/sh_probes.shpr) is the
cross-language golden fixture: origin `(1,2,3)`, spacing `(4,5,6)`, dimensions
`(2,1,1)`, scale `0.25`, stored integers `-12..11` in wire order,
and inside bits `(true,false)`.
