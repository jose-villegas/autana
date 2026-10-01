/*
 * asset_pack: a content pack used where it lies, in a memory-mapped flash
 * partition or in a buffer read from a file. A pack is a header, a table of
 * named entries and their bytes; the layout is in docs/Asset-Packs.md and is
 * written by launcher/tools/r3d/asset_pack.py. An entry is a typed byte range
 * that refers to its own parts by offset from its first byte, so no entry
 * needs a fix-up pass and the pack is never copied.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ASSET_PACK_MAGIC       "APAK"
#define ASSET_PACK_VERSION     1u
#define ASSET_PACK_HEADER_SIZE 32u
#define ASSET_PACK_ENTRY_SIZE  48u
#define ASSET_NAME_MAX         32u /* including the NUL */

typedef enum {
    ASSET_OK = 0,
    ASSET_ERR_NO_PACK,   /* nothing mapped or read */
    ASSET_ERR_TRUNCATED, /* the buffer is shorter than the pack's own header, or than the size it states */
    ASSET_ERR_MAGIC,     /* not an asset pack */
    ASSET_ERR_VERSION,   /* a format this firmware does not read */
    ASSET_ERR_SIZE,      /* the stated size cannot hold a header */
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

/* CRC-32 (zlib's), which is the checksum a pack carries over its bytes after the header. */
uint32_t asset_crc32(const void* data, size_t size);

/* Checks the header, the size, the checksum and every entry's range and
 * alignment, then fills `pack`. */
asset_status_t asset_pack_open(asset_pack_t* pack, const void* base, size_t size);

/* The entry named `name`, which must be of `type`. */
asset_status_t asset_pack_find(const asset_pack_t* pack, const char* name, uint32_t type, asset_view_t* view);

/* A short phrase for a status, for a log line. */
const char* asset_status_text(asset_status_t status);
