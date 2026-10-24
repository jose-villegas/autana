# name_hash

Stable numeric keys for finding things by name with a number compare. Hash once
at initialization or load time, then compare stored keys during lookup.

`name_hash()` hashes a NUL-terminated name; `name_hash_n()` hashes a byte span,
including embedded NULs. Both use FNV-1a 32 (Fowler, Noll, Vo). The package
requires only libc; `NAME_HASH_NONE` is reserved for absent names.

`name_hash/name_hash64.h` provides `name_hash64_n(seed, data, size)` for 64-bit
byte-span fingerprints. Start with `NAME_HASH64_OFFSET_BASIS` or continue a previous fold.

Include `"name_hash/name_hash.h"`. Firmware declares `REQUIRES name_hash`;
plain CMake builds link the `name_hash` target. Host scripts discover sources
and suites through `launcher/tools/build/packages.sh`.

The C and Python tests share `tests/name_hash_vectors.h`.

## Headers

<!-- generated: package-name-hash-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/name_hash/name_hash.h](include/name_hash/name_hash.h) | Names become stable numeric keys for lookup at initialization or load time. |
| [include/name_hash/name_hash64.h](include/name_hash/name_hash64.h) | Seeded 64-bit name hashes support incremental byte-span fingerprints. |
<!-- /generated: package-name-hash-headers -->
