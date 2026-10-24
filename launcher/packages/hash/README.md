# hash

Hashes and checksums over bytes, one owner for each; the package requires only
libc. Name hashes are stable numeric keys: hash once at initialization or load
time, then compare stored keys during lookup.

`name_hash()` hashes a NUL-terminated name; `name_hash_n()` hashes a byte span,
including embedded NULs. Both use FNV-1a 32 (Fowler, Noll, Vo).
`NAME_HASH_NONE` is reserved for absent names. `name_hash64_n(seed, data, size)`
starts from `NAME_HASH64_OFFSET_BASIS` or continues a previous fold.
`crc32_update(crc, data, size)` starts from 0 or continues a previous result,
as zlib's `crc32()` does.

Include `"hash/<name>.h"`. Firmware declares `REQUIRES hash`; plain CMake
builds link the `hash` target. Host scripts discover sources and suites through
`launcher/tools/build/packages.sh`.

The C and Python tests share `tests/name_hash_vectors.h`.

## Headers

<!-- generated: package-hash-headers check: python launcher/tools/gen/shared_helpers.py --check -->
| Header | Purpose |
|---|---|
| [include/hash/crc32.h](include/hash/crc32.h) | crc32: the CRC-32 of RFC 1952, which zlib and PNG compute, a nibble at a time, so no 1 KiB table sits in RAM. |
| [include/hash/name_hash.h](include/hash/name_hash.h) | Names become stable numeric keys for lookup at initialization or load time. |
| [include/hash/name_hash64.h](include/hash/name_hash64.h) | Seeded 64-bit name hashes support incremental byte-span fingerprints. |
<!-- /generated: package-hash-headers -->
