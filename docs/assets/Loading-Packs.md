# Loading packs

Part of the [asset docs](README.md). A pack is mounted by name, an entry is
found in it by name and type, and the bytes are read where they lie.

## Async loading

Request a pack in `enter()` and read its state each frame.

<!-- generated: asset-load-pack-async-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* load_pack_async: request on enter, read each frame, release on exit. */
#include "asset/asset_store.h"

typedef struct {
    const char* name;
    handle_t load;
} load_pack_example_t;

void
load_pack_enter(load_pack_example_t* visit, const char* name) {
    *visit = (load_pack_example_t){.name = name, .load = asset_store_load_async(name, NULL, HANDLE_NONE)};
}

const asset_pack_t*
load_pack_frame(const load_pack_example_t* visit) {
    return asset_store_loaded(visit->load);
}

void
load_pack_exit(load_pack_example_t* visit) {
    if (asset_store_state(visit->load) == PENDING_READY) {
        asset_store_release(visit->name);
    } else {
        asset_store_cancel(visit->load);
    }
    visit->load = HANDLE_NONE;
}
```
<!-- /generated: asset-load-pack-async-example -->

On exit, release(name) a READY request and cancel any other.
`asset_store_loaded(load)` is NULL unless the request is READY.
A failed request logs one reason; leave that content out.

The async call returns a generational handle; `done` may be NULL to poll.
Callbacks run once on core 0 in the
[frame-loop poll phase](../firmware/Frame-Loop.md#engine-systems), before
`frame()`, never inside the request call.

- An already mounted pack is READY immediately; its callback waits for `poll`.
- Requests made inside a callback wait for the next pass.
- Pending mounts share `ASSET_STORE_STEP_BYTES` per pass for CRC bytes and
  entry-table checks. Requests for one pack share validation
  through [`asset_pack_open_step()`](../../launcher/main/asset/asset_pack.h).

| State | Lifetime |
|---|---|
| WAIT | Until validation completes or the request is cancelled |
| READY | Holds one pack use until released |
| FAILED | Remains readable until pool pressure reclaims it, oldest first |
| CANCELLED | Frees its partial mount and is immediately reusable; no callback |
| NONE | A zero, stale, released or unknown handle |

`asset_store_cancel(load)` suppresses completion; READY keeps its use.
A token from `pending_token_new()` groups requests. Dead owners are
cancelled silently, at the request or on the next poll.

`HANDLE_NONE` owns requests the loop cancels when the app exits.
App exit also cancels every live token, after the app's `exit()`.
Blocking uses remain held.

## Blocking loading

<!-- generated: asset-load-pack-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* load_pack: read one entry while holding a counted pack use. */
#include "asset/asset_store.h"

/* Blocking form: small packs, startup and tests. */
asset_status_t
load_pack_example(const char* pack_name, const char* entry, uint32_t type, uint32_t* size) {
    const asset_pack_t* pack = asset_store_load(pack_name);
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

`asset_store_load(name)` drives a local request inline, outside the async pool.
Missing or bad packs give NULL plus one log line. It never delivers another
request's callback while waiting.

## Counted mounts

Both forms hold one pack use on success. `asset_store_release(name)` drops
one use; at none the pack is unmapped or freed. `asset_store_mounted()` counts
mounted packs, and `ASSET_STORE_MOUNTS_MAX` bounds the table.

Release drops a blocking use first; otherwise it retires the oldest READY request for that name.
The engine pack keeps its slot for the run.

`scene_load(id)` opens the scene and its dependencies synchronously;
`scene_unload()` releases its pack. A scene that fails to load is not drawn.

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
none, and validates it. The store reads the same bytes through a resumable
`asset_pack_open_step()` validation path. Its `dir` is
`AUTANA_ASSET_DIR`, set per process; `run_tests.sh` and the render scripts
write the packs in the tree and set it, or build the folder into the
renderer.
