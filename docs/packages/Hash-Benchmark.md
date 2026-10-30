# Hash benchmark

CRC-32 candidates read the same bytes from internal RAM and flash-mapped
rodata. Each row times one checksum call, using testkit's warm and cold
cycle medians and instruction counter. QEMU does not report board timing.

Capture with `autana suite hash_perf --flash`. Keep the capture's `BUILD_ID`,
`HASHTARGET` and `HASHBENCH` lines in [the capture](data/hash_bench_capture.txt),
then run `python launcher/tools/perf/hash_bench_table.py`.

The [fast-path contract](Fast-Paths.md#crc32-update) links the correctness tests.
A dash marks a pending board measurement.

<!-- generated: hash-benchmark check: python launcher/tools/perf/hash_bench_table.py --check -->
Build: `-`; target: -.

| Candidate | Bytes | Source | Warm cycles | Cold cycles | Instructions |
|---|---|---|---|---|---|
| `crc32_update_nibble` | 64 | ram | - | - | - |
| `crc32_update_nibble` | 64 | flash | - | - | - |
| `crc32_update_nibble` | 4096 | ram | - | - | - |
| `crc32_update_nibble` | 4096 | flash | - | - | - |
| `crc32_update_nibble` | 65536 | ram | - | - | - |
| `crc32_update_nibble` | 65536 | flash | - | - | - |
| `crc32_update_slice4` | 64 | ram | - | - | - |
| `crc32_update_slice4` | 64 | flash | - | - | - |
| `crc32_update_slice4` | 4096 | ram | - | - | - |
| `crc32_update_slice4` | 4096 | flash | - | - | - |
| `crc32_update_slice4` | 65536 | ram | - | - | - |
| `crc32_update_slice4` | 65536 | flash | - | - | - |
| `crc32_update_rom` | 64 | ram | - | - | - |
| `crc32_update_rom` | 64 | flash | - | - | - |
| `crc32_update_rom` | 4096 | ram | - | - | - |
| `crc32_update_rom` | 4096 | flash | - | - | - |
| `crc32_update_rom` | 65536 | ram | - | - | - |
| `crc32_update_rom` | 65536 | flash | - | - | - |
<!-- /generated: hash-benchmark -->
