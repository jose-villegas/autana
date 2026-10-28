# ABDR v2 pack directory

A region that holds several packs, the device's partition, starts with a
directory that says where each lies. Layout defines:
[`asset/asset_directory_format.h`](../../../launcher/main/asset/asset_directory_format.h).

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Header, 16 bytes at 0, one cell a byte
0-3: "magic ABDR"
4-7: "crc32"
8-11: "version = 2"
12-15: "pack_count"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #f4a885 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #93d6b0 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 44
---
packet-beta
title Pack registry, one 40-byte row a pack from byte 16
0-31: "name, NUL padded"
32-35: "offset"
36-39: "size"
```

| Part | At | Size |
|---|---|---|
| Header | 0 | 16 bytes |
| Pack registry | 16 | 40 bytes a pack |
| Content: packs | each row's `offset` | each row's `size`; each pack starts on its own MMU page, after the registry, in row order |

| Rule | Detail |
|---|---|
| Owners | `ASSET_DIRECTORY_MAGIC`, `_VERSION`, `_HEADER_SIZE` (16), `_ROW_SIZE` (40) and `ASSET_MMU_PAGE_SIZE` (65536) in `asset_directory_format.h`; Python reads them with `read_define()` |
| Header | `crc32` is zlib's CRC-32 of the bytes from 8 (the version) to the registry's end |
| Rows | name nonempty with room for its NUL, unique; `offset` a multiple of `ASSET_MMU_PAGE_SIZE`, past the registry and the previous pack; `offset + size` inside the region |
| Pages | the partition and every pack start on MMU pages, so releasing one mapping cannot invalidate another; padding costs less than a page a pack |
| Order | header, registry, then the packs in row order; gaps zero |
