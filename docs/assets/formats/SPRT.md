# SPRT v1 sprite

A picture with a nine-slice border, read in place ([Image-Entries.md](../Image-Entries.md#sprites)). Layout defines: [`draw/draw_image_format.h`](../../../launcher/main/draw/draw_image_format.h).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(8) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(9) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 58
---
packet-beta
title Header, 12 bytes at 0, one cell a byte
0-1: "version = 1"
2: "left"
3: "top"
4: "right"
5: "bottom"
6: "flat"
7: "centre alpha"
8-9: "centre colour"
10-11: "reserved = 0"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 12 bytes |
| Picture | 12 | the rest of the entry: an [IMAG](IMAG.md) entry whole, format 1 or 3, its offsets counted from its own start |

| Rule | Detail |
|---|---|
| Owners | `DRAW_SPRITE_ASSET`, `_VERSION`, `_HEADER_SIZE` (12), `_AT_BORDER` (2), `_AT_FLAT` (6), `_AT_CENTRE_ALPHA` (7), `_AT_CENTRE_COLOR` (8), `_AT_RESERVED` (10) in `draw_image_format.h` |
| Border | pixels of the stored picture kept 1:1 at each edge; `left + right` below its width and `top + bottom` below its height, so the centre is at least one pixel |
| Flat centre | `flat` 1 when every pixel inside the border has one alpha and, where that alpha is above 0, one colour: `centre colour` (byte-swapped RGB565) at `centre alpha`; else 0 and both are 0 |
| Picture | 4-aligned, since the header is 12 bytes and an entry starts 16-aligned |
| Name | the `.image.toml`'s stem, like an IMAG; one name holds one entry, a SPRT or an IMAG |
| Errors | `ASSET_ERR_VERSION` for another version; `ASSET_ERR_FORMAT` for `flat` above 1, reserved bytes in use or a one-bit picture; `ASSET_ERR_BOUNDS` for an entry shorter than the header or a border that leaves no centre; the picture's own errors as IMAG |
| View | the planes `draw_sprite_open()` returns point into the entry |
