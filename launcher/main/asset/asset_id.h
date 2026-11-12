/* asset_id: an id that names an entry and, optionally, the pack holding it,
 * "pack/name" or a bare "name"; the caller decides which pack a bare id means. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "asset/asset_pack.h"

/* The separator between an id's pack and its name. */
#define ASSET_ID_PACK_SEPARATOR '/'

/* An id's parts: the pack's name is the id's first `pack_length` bytes, 0 for
 * a bare id; `name` points into the id. */
typedef struct {
    size_t pack_length;
    const char* name;
} asset_id_t;

/* Splits `id`; false when a part is empty or does not fit ASSET_NAME_MAX, or
 * the name holds a second separator. */
static inline bool
asset_id_split(const char* id, asset_id_t* out) {
    const char* separator = strchr(id, ASSET_ID_PACK_SEPARATOR);
    out->pack_length = separator == NULL ? 0 : (size_t)(separator - id);
    out->name = separator == NULL ? id : separator + 1;
    return (separator == NULL || (out->pack_length > 0 && out->pack_length < ASSET_NAME_MAX)) && out->name[0] != '\0'
           && strlen(out->name) < ASSET_NAME_MAX && strchr(out->name, ASSET_ID_PACK_SEPARATOR) == NULL;
}

/* The pack `parts` names in `id`, or `bare_pack` for a bare id, into `out`. */
static inline void
asset_id_pack(const char* id, asset_id_t parts, const char* bare_pack, char out[ASSET_NAME_MAX]) {
    const char* pack = parts.pack_length == 0 ? bare_pack : id;
    const size_t length = parts.pack_length == 0 ? strlen(bare_pack) : parts.pack_length;
    (void)memcpy(out, pack, length);
    out[length] = '\0';
}
