/*
 * asset_pack: a content pack used where it lies, in a memory-mapped flash
 * partition or in a buffer read from a file or a card. A pack is a header, a
 * table of named entries and their bytes; the layout is in docs/assets/README.md
 * and is written by launcher/tools/asset/asset_pack.py. An entry is a typed
 * byte range that refers to its own parts by offset from its first byte, so no
 * entry needs a fix-up pass and the pack is never copied.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASSET_PACK_MAGIC       "APAK"
#define ASSET_PACK_VERSION     1U
#define ASSET_PACK_HEADER_SIZE 32U
#define ASSET_PACK_ENTRY_SIZE  48U
#define ASSET_NAME_MAX         32U /* including the NUL */

/* An entry type is four characters, stored so a hex dump reads them. The
 * module that owns a kind of content defines its own, so there is no central
 * list to keep. */
#define ASSET_TYPE(a, b, c, d)                                                                                         \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16)                           \
     | ((uint32_t)(uint8_t)(d) << 24))

typedef enum {
    ASSET_OK = 0,
    ASSET_ERR_NO_PACK,   /* nothing mapped or read */
    ASSET_ERR_TRUNCATED, /* the buffer is shorter than the pack's own header, or than the size it states */
    ASSET_ERR_MAGIC,     /* not an asset pack */
    ASSET_ERR_VERSION,   /* a format this firmware does not read */
    ASSET_ERR_SIZE,      /* the header is malformed: a size that cannot hold it, or reserved bytes in use */
    ASSET_ERR_CRC,       /* the bytes after the header do not match its checksum */
    ASSET_ERR_BOUNDS,    /* an entry or one of its parts leaves its range, or is misaligned */
    ASSET_ERR_NOT_FOUND, /* no entry has that name */
    ASSET_ERR_TYPE,      /* the entry is not of the type asked for */
} asset_status_t;

/* A validated pack. Holds no copy: `base` must outlive it. */
typedef struct {
    const uint8_t* base;
    uint32_t size;
    uint32_t count;
} asset_pack_t;

/* One entry's bytes, in place. */
typedef struct {
    const uint8_t* data;
    uint32_t size;
} asset_view_t;

/* One row of the entry table. `name` points into the pack, NUL padded. */
typedef struct {
    const char* name;
    uint32_t type;
    asset_view_t view;
} asset_entry_t;

/* CRC-32 (zlib's), which is the checksum a pack carries over its bytes after the header. */
uint32_t asset_crc32(const void* data, size_t size);

/* The size a pack states in its header, from at least ASSET_PACK_HEADER_SIZE
 * bytes at `head`; 0 when they are not an asset pack's. How much to read or
 * map before opening it. */
uint32_t asset_pack_total_size(const void* head, size_t available);

/* Checks the header, the checksum and every entry's range and alignment,
 * then fills `pack`. `base` must be 16-byte aligned, which a partition
 * mapping and aligned_alloc() both give. A buffer may be larger than the pack. */
asset_status_t asset_pack_open(asset_pack_t* pack, const void* base, size_t size);

/* Entry `index` of the table, the one place a row's offset becomes bytes. */
asset_status_t asset_pack_entry(const asset_pack_t* pack, uint32_t index, asset_entry_t* entry);

/* The entry named `name`, which must be of `type`. */
asset_status_t asset_pack_find(const asset_pack_t* pack, const char* name, uint32_t type, asset_view_t* view);

/* A short phrase for a status, for a log line. */
const char* asset_status_text(asset_status_t status);
