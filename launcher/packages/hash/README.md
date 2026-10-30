# hash

Hashes and checksums over bytes, one owner for each; the package requires only
libc. Name hashes are stable numeric keys: hash once at initialization or load
time, then compare stored keys during lookup.

`name_hash()` hashes a NUL-terminated name; `name_hash_n()` hashes a byte span,
including embedded NULs. Both use FNV-1a 32 (Fowler, Noll, Vo).
`NAME_HASH_NONE` is reserved for absent names. `name_hash64_n(seed, data, size)`
starts from `NAME_HASH64_OFFSET_BASIS` or continues a previous fold.
`crc32_update(crc, data, size)` starts from 0 or continues a previous result,
as zlib's `crc32()` does. `crc32_update_nibble()` is the reference;
`crc32_update_slice4()` uses flash-resident tables. Firmware also provides
`crc32_update_rom()` through the chip's ROM linker symbol.

The Hash Kconfig choice selects ROM (default), slice-by-4 or nibble.
Hosts use nibble for `crc32_update()` and test both portable candidates by name.
All candidates preserve arbitrary seeds and chunk boundaries exactly.

The ROM entry complements the seed and result, matching zlib directly.
The wrapper returns the seed for an empty span, including a null pointer.
[Candidate tests](tests/suite_crc32.c) compare random spans, unaligned starts,
nonzero seeds and incremental updates against the nibble reference.

[Benchmark capture](../../../docs/packages/Hash-Benchmark.md) covers
internal RAM and flash-mapped input. [Fast paths](../../../docs/packages/Fast-Paths.md#crc32-update)
records the named bit-error limit.

Include `"hash/<name>.h"`. Firmware declares `REQUIRES hash`; plain CMake
builds link the `hash` target. Host scripts discover sources and suites through
`launcher/tools/build/packages.sh`.

The C and Python tests share `tests/name_hash_vectors.h`.

## Headers

<!-- generated: package-hash-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/hash/crc32.h](include/hash/crc32.h) | crc32: crc32_update computes incremental RFC 1952 checksums; Kconfig HASH_CRC32_* selects the implementation. |
| [include/hash/name_hash.h](include/hash/name_hash.h) | Names become stable numeric keys for lookup at initialization or load time. |
| [include/hash/name_hash64.h](include/hash/name_hash64.h) | Seeded 64-bit name hashes support incremental byte-span fingerprints. |
<!-- /generated: package-hash-headers -->
