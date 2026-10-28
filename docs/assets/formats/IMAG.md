# IMAG v1 picture

A picture, read in place ([Image-Entries.md](../Image-Entries.md)). Layout defines: [`draw/draw_image_format.h`](../../../launcher/main/draw/draw_image_format.h).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #f4a885 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Header, 16 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "format"
4-5: "width"
6-7: "height"
8-11: "stride"
12-15: "pixels_off"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 4
    bitWidth: 120
---
packet-beta
title Content: one RGB565 pixel (format 1), 2 bytes from pixels_off
0-1: "gfx_color_t, byte-swapped RGB565"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 2
    bitWidth: 240
---
packet-beta
title Content: eight 1-bit pixels (format 2), 1 byte from pixels_off
0: "bit 7 leftmost pixel, 1 = ink"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 16 bytes |
| Content: pixel rows | `pixels_off` | `stride * (height - 1) + width` pixels, top row first; 2 bytes each in format 1, one bit each in format 2 |

| Rule | Detail |
|---|---|
| Owners | `DRAW_IMAGE_VERSION`, `_FORMAT_RGB565` (1), `_FORMAT_MONO1` (2, for [icons](../../tools/Icon-Baker.md)), `_HEADER_SIZE` (16), `_PIXEL_ALIGN` (4), `_MONO1_STRIDE_STEP` (8) in `draw_image_format.h` |
| Size | `width` and `height` are 1 to 65535 |
| Stride | pixels from one row's start to the next, at least `width`; a multiple of `DRAW_IMAGE_MONO1_STRIDE_STEP` in format 2, so a row is whole bytes |
| Pixels | `pixels_off` at least the header size; 4-aligned in format 1 |
| Errors | `ASSET_ERR_VERSION` for another version; `ASSET_ERR_FORMAT` for an unknown format, an empty picture, a short stride, a stride off its step or `pixels_off` inside the header; `ASSET_ERR_BOUNDS` for rows that leave the entry or misaligned RGB565 rows |
| View | the pixels `draw_image_open()` returns point into the entry |
