# Loading packs

Part of the [asset docs](README.md). A pack is mounted by name, an entry is
found in it by name and type, and the bytes are read where they lie.

## The store

`asset_store_pack(name)` mounts pack `name` and checks it on its first
use, then counts uses; `asset_store_release(name)` drops one, and at none the
pack is unmapped or freed. A missing or bad pack is `NULL` and one log
line saying why. At most `ASSET_STORE_MOUNTS_MAX` packs are mounted at once,
and the engine pack, mounted when the first system icon is drawn, keeps its
slot for the run; `asset_store_mounted()` counts them.

`scene_load(id)` mounts pack `id` and opens its scene entry `id`, every mesh,
skin, skeleton and clip it names from it, failing on the first missing or
malformed one with the id and the status; `scene_unload()` releases it. A scene
that fails to load is not drawn; showing that is up to the app.

## The device

`launcher/partitions.csv` holds the packs in a data partition labelled
`assets`, subtype `0x40` (the first application subtype), at the end of the
16 MB flash.

| Partition | Offset | Size |
|---|---|---|
| `nvs` | `0x9000` | `0x6000` |
| `phy_init` | `0xF000` | `0x1000` |
| `factory` (the app) | `0x10000` | `0x7F0000` |
| `coredump` | `0x800000` | `0x10000` |
| `assets` | `0x810000` | `0x7F0000` |

On the first mount the store finds the partition and reads its directory into
RAM, kept for good. Each pack is then mapped alone from its slot. The
directory copy holds no MMU page and keeps enumeration independent of pack
mounts. Reads of a mapped
pack go through the flash cache as the app's own const data does, so an entry
costs no RAM and no copy. When the partition table is older than the firmware,
the directory is missing or a check fails, the log says why and the content
that asked stays out.

## The host

The host lays packs out the way a card will: one file per pack,
`<dir>/<name>.apak`. `asset_file_open()` reads a file into aligned memory
outside the host tests' modelled heap, as a mapped partition costs the board
none, and calls the same `asset_pack_open()`. The store's `dir` is
`AUTANA_ASSET_DIR`, set per process; `run_tests.sh` and the render scripts
write the packs in the tree and set it, or build the folder into the
renderer.

## Example

Mount a pack, find one entry and release the pack. The entry's bytes point into the pack, so a caller uses them before the release.

<!-- generated: asset-load-pack-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Load a pack: mount it, find one entry, release it. */
#include "asset/asset_store.h"

asset_status_t
load_pack_example(const char* pack_name, const char* entry, uint32_t type, uint32_t* size) {
    const asset_pack_t* pack = asset_store_pack(pack_name);
    if (pack == NULL) {
        return ASSET_ERR_NO_PACK; /* the log line says why: missing, bad CRC, store full */
    }
    asset_view_t view;
    const asset_status_t status = asset_pack_find(pack, entry, type, &view);
    if (status == ASSET_OK) {
        *size = view.size; /* view.data points into the pack: use it before the release */
    }
    asset_store_release(pack_name);
    return status;
}
```
<!-- /generated: asset-load-pack-example -->
