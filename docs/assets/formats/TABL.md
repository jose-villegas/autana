# TABL v1 table

Named rows of int16 values sharing one fractional-bit count ([Table-Entries.md](../Table-Entries.md)). Layout defines: [`asset/asset_table_format.h`](../../../launcher/main/asset/asset_table_format.h).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #ffffff !important; stroke-dasharray: 4 3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 84
---
packet-beta
title Header, 8 bytes at 0, one cell a byte
0-1: "version = 1"
2-3: "rows"
4-5: "columns"
6: "frac"
7: "0"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 14
    bitWidth: 56
---
packet-beta
title Content: row, two columns shown, 24 + 2 bytes a column from byte 8
0-23: "name, NUL padded"
24-25: "int16 value[0]"
26-27: "int16 value[1]"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 8 bytes |
| Content: rows | `8 + i * (24 + 2 * columns)` | `24 + 2 * columns` bytes a row, in the order written |

| Rule | Detail |
|---|---|
| Owners | `ASSET_TABLE_VERSION`, `_HEADER_SIZE` (8), `_NAME_SIZE` (24) in `asset_table_format.h` |
| Header | `frac` is the fractional bits of every value (0 to 255); the reserved byte is zero |
| Dimensions | 1 to 65535 rows and columns; the entry is exactly `8 + rows * (24 + 2 * columns)` bytes |
| Names | at most 23 UTF-8 bytes, then the NUL, in the 24-byte field |
| Values | int16, `VALUE_MIN` to `VALUE_MAX` in `table_asset.py` |
| Errors | `asset_table_open()`: `ASSET_ERR_BOUNDS` for an unaligned (2-byte) base or a short header; `ASSET_ERR_VERSION`; `ASSET_ERR_FORMAT` for empty dimensions, the reserved byte, a size off by a byte or an unterminated name. `asset_table_row()`: `ASSET_ERR_FORMAT` for another column count, `ASSET_ERR_NOT_FOUND` for an absent name |
| View | values point into the mounted pack; the reader allocates nothing |
