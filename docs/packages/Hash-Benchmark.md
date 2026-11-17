# Hash benchmark

CRC-32 candidates read the same bytes from internal RAM and flash-mapped
rodata. Each row times one checksum call, using testkit's warm and cold
cycle medians and instruction counter. QEMU does not report board timing.

Capture with `autana suite run_hash_perf_suite --flash`. Keep the capture's `BUILD_ID`,
`HASHTARGET` and `HASHBENCH` lines in [the capture](data/hash_bench_capture.txt),
then run `python launcher/tools/perf/hash_bench_table.py`.

The [fast-path contract](Fast-Paths.md#crc32-update) links the correctness tests.
The table contains the rows present in the capture.

<!-- generated: hash-benchmark check: python launcher/tools/perf/hash_bench_table.py --check -->
Target: esp32s3 (Xtensa, LX7_ESP32_S3_MP).

| Candidate | Bytes | Source | Warm cycles | Cold cycles | Instructions |
|---|---|---|---|---|---|
| `crc32_update_nibble` | 64 | flash | 1105.0 | 2639.0 | 999.0 |
| `crc32_update_nibble` | 64 | ram | 1105.0 | 2552.0 | 999.0 |
| `crc32_update_nibble` | 4096 | flash | 69674.0 | 77546.0 | 61479.0 |
| `crc32_update_nibble` | 4096 | ram | 69674.0 | 71337.0 | 61479.0 |
| `crc32_update_nibble` | 16384 | flash | 281365.0 | 309319.0 | 246552.0 |
| `crc32_update_nibble` | 16384 | ram | 280036.0 | 283037.0 | 247679.0 |
| `crc32_update_rom` | 64 | flash | 677.0 | 1492.0 | 646.0 |
| `crc32_update_rom` | 64 | ram | 677.0 | 1048.0 | 646.0 |
| `crc32_update_rom` | 4096 | flash | 40491.0 | 47074.0 | 37942.0 |
| `crc32_update_rom` | 4096 | ram | 40491.0 | 40864.0 | 37942.0 |
| `crc32_update_rom` | 16384 | flash | 163324.0 | 187704.0 | 152379.0 |
| `crc32_update_rom` | 16384 | ram | 163323.0 | 163691.0 | 151606.0 |
<!-- /generated: hash-benchmark -->
