# Hash benchmark

CRC-32 candidates read the same bytes from internal RAM and flash-mapped
rodata. Each row times one checksum call, using testkit's warm and cold
cycle medians and instruction counter. QEMU does not report board timing.

Capture with `autana suite hash_perf --flash`. Keep the capture's `BUILD_ID`,
`HASHTARGET` and `HASHBENCH` lines in [the capture](data/hash_bench_capture.txt),
then run `python launcher/tools/perf/hash_bench_table.py`.

The [fast-path contract](Fast-Paths.md#crc32-update) links the correctness tests.
The table contains the rows present in the capture.

<!-- generated: hash-benchmark check: python launcher/tools/perf/hash_bench_table.py --check -->
Build: `-`; target: -.

Capture pending.

| Candidate | Bytes | Source | Warm cycles | Cold cycles | Instructions |
|---|---|---|---|---|---|
<!-- /generated: hash-benchmark -->
