# Math benchmark

The board supplies cycles and retired instructions. QEMU supplies instruction
counts for every row as information, without cache stalls or FPU latency.
Only float dot, normalize, mat4 mul and slerp are guarded by the CI ratchet.
The QEMU column marks wrappers containing `recip0.s`/`rsqrt0.s` as
"device-only instruction": `CONFIG_LAUNCHER_QEMU` selects `1 / a` and
`1 / sqrtf(a)` instead of the device's seed instructions and Newton corrections.

Max error comes from the `suite_math_numbers` board run, appended to the capture.
Its `MATHF_RSQRT` lines are exhaustive proofs; its `MATHERROR` lines are
normalize and nlerp component errors against the exact path, over a sweep.

Warm is the median after an untimed call, following Embench-IoT's cache-warming
method.<sup>[[69]](../Citations.md#69)</sup> Cold is the median per call after
I-cache and flash-rodata D-cache invalidation through IDF's `esp_cache_msync()`,
with caches enabled and PSRAM untouched. The linker-defined flash text and rodata
ranges are rounded to their cache-line boundaries and invalidated without
writeback, including libm and other callees. Inputs and outputs are in internal
RAM; cold calls refill code and flash rodata.
Board instructions count a warmed call on core zero with frame counters disarmed;
interrupts can contribute. A compiler barrier preserves all output components.
Mat4 rows include the struct copies of the by-value API.

An op is one input element, except `dot_f32_n256`, whose op is a full dot product
call matching ESP-DSP's ANSI O2 workload.<sup>[[68]](../Citations.md#68)</sup>
The calibration below compares warm cycles per call; compiler flags, loads,
stores and call overhead affect the comparison. QEMU counts per wrapper call
are divided by the row's op count. Cycles/insn uses warm cycles and board instructions.

Types follow the [math suffixes](README.md#number-types); conversions name source
and destination. Transform compute_matrix composes TRS; quat mul composes rotations.
The fused/approx column lists matching wrapper mnemonics and sorted callees,
without surveying callee bodies. An unresolved indirect target is a starred
register; `-` means no matching instruction or call.

Record `BUILD_ID=` and `MATHBENCH` lines from
`autana suite run_math_perf_suite --flash --perf-scope` in
`data/math_bench_capture.txt`. Record wrapper metadata and instructions from the
same build's ELF with `python launcher/tools/perf/math_bench_table.py --from-elf <launcher.elf>`.
The ELF manifest and capture carry row labels, types and op counts.
Run `python launcher/tools/perf/math_bench_table.py` to update the block, or add
`--check` to verify it without writing.

<!-- generated: math-benchmark check: python launcher/tools/perf/math_bench_table.py --check -->
Board build: `2a01f8108518-diag`.

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| add (vec3) | f | 15.3 | 2116.0 | 14.5 | 14.2 | 1.1 | - | - |
| add (vec3) | i | 14.3 | 1858.0 | 14.5 | 14.2 | 1.0 | - | - |
| add (vec3) | s | 37.4 | 4324.0 | 31.6 | 31.4 | 1.2 | - | - |
| add (vec3) | x | 58.4 | 6529.0 | 44.6 | 44.4 | 1.3 | - | - |
| atan2f | f | 336.7 | 29770.0 | 195.9 | 197.0 | 1.7 | calls atan2f | - |
| convert | f_to_i | 95.5 | 9187.0 | 62.1 | 62.0 | 1.5 | - | - |
| convert | f_to_s | 104.5 | 9994.0 | 75.6 | 75.5 | 1.4 | - | - |
| convert | f_to_x | 110.6 | 10348.0 | 70.7 | 70.5 | 1.6 | - | - |
| convert | i_to_f | 11.3 | 1876.0 | 11.5 | 11.2 | 1.0 | - | - |
| convert | s_to_f | 14.4 | 2065.0 | 14.5 | 14.3 | 1.0 | - | - |
| convert | x_to_f | 14.3 | 2050.0 | 14.5 | 14.3 | 1.0 | - | - |
| cosf | f | 191.5 | 21487.0 | 110.7 | 111.6 | 1.7 | calls _cosf | - |
| div_recip | f | 58.3 | 5125.0 | 40.5 | 40.3 | 1.4 | calls __divsf3 | - |
| div_rsqrt | f | 114.2 | 10069.0 | 81.5 | 82.3 | 1.4 | calls __divsf3, sqrtf | - |
| dot (vec3) | f | 17.3 | 2254.0 | 12.5 | 12.2 | 1.4 | madd.s | - |
| dot_f32_n256 | f | 1305.0 | 2467.0 | 1315.0 | 1299.0 | 1.0 | madd.s | - |
| dot (vec3) | i | 24.3 | 2899.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | s | 24.3 | 2905.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | x | 83.8 | 9694.0 | 65.9 | 65.8 | 1.3 | - | - |
| lerp (vec3) | f | 19.3 | 2635.0 | 17.5 | 17.3 | 1.1 | madd.s | - |
| lerp (vec3) | i | 101.3 | 9190.0 | 93.5 | 93.4 | 1.1 | - | - |
| lerp (vec3) | s | 33.3 | 3706.0 | 31.5 | 31.3 | 1.1 | - | - |
| lerp (vec3) | x | 127.4 | 12877.0 | 99.5 | 99.5 | 1.3 | - | - |
| mat4 mul | f | 738.4 | 49291.0 | 550.5 | 506.4 | 1.3 | madd.s; calls memcpy | - |
| mat4 mul | x | 2389.4 | 159370.0 | 1655.1 | 1640.4 | 1.4 | calls memcpy | - |
| mathf_recip | f | 26.3 | 3058.0 | 13.5 | device-only instruction | 1.9 | madd.s msub.s recip0.s | 1 ulp |
| mathf_rsqrt_approx | f | 24.3 | 2947.0 | 12.5 | device-only instruction | 1.9 | msub.s rsqrt0.s | 5.94258e-05 relative (proven, exhaustive) |
| mathf_rsqrt | f | 38.4 | 4372.0 | 18.6 | device-only instruction | 2.1 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 ulp (proven, exhaustive) |
| mul (scalar) | f | 9.3 | 1537.0 | 6.5 | 6.2 | 1.4 | - | - |
| mul (scalar) | i | 9.3 | 1549.0 | 8.5 | 8.2 | 1.1 | - | - |
| mul (scalar) | s | 11.3 | 2332.0 | 10.5 | 10.2 | 1.1 | - | - |
| mul (scalar) | x | 22.3 | 3223.0 | 17.5 | 17.3 | 1.3 | - | - |
| nlerp | f | 94.8 | 8761.0 | 53.7 | device-only instruction | 1.8 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 2 ulp (measured, sweep) |
| nlerp | x | 1172.8 | 91072.0 | 920.1 | 909.9 | 1.3 | calls __divdi3 | - |
| normalize (vec3) | f | 57.4 | 5866.0 | 29.9 | device-only instruction | 1.9 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 ulp (measured, sweep) |
| normalize (vec3) | x | 1127.1 | 77746.0 | 931.6 | 681.2 | 1.2 | calls __udivdi3 | - |
| quat mul | f | 30.3 | 3277.0 | 30.5 | 30.2 | 1.0 | madd.s msub.s | - |
| quat mul | x | 419.1 | 40174.0 | 313.5 | 313.9 | 1.3 | - | - |
| scale (vec3) | f | 13.3 | 2020.0 | 12.5 | 12.2 | 1.1 | - | - |
| scale (vec3) | i | 12.3 | 1933.0 | 12.5 | 12.2 | 1.0 | - | - |
| scale (vec3) | s | 36.4 | 4300.0 | 30.6 | 30.4 | 1.2 | - | - |
| scale (vec3) | x | 66.0 | 7996.0 | 49.2 | 49.0 | 1.3 | - | - |
| sinf | f | 191.4 | 22222.0 | 109.1 | 110.0 | 1.8 | calls _sinf | - |
| slerp | f | 452.7 | 34576.0 | 306.3 | device-only instruction | 1.5 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | - |
| sqrtf | f | 61.2 | 6436.0 | 45.5 | 46.3 | 1.3 | calls sqrtf | - |
| transform compute_matrix | f | 67.4 | 7027.0 | 67.6 | 67.4 | 1.0 | madd.s msub.s | - |
| transform compute_matrix | x | 854.0 | 76513.0 | 660.2 | 661.4 | 1.3 | calls mat4x_from_trs, memcpy | - |
| trig_sin | i | 30.8 | 5233.0 | 24.0 | 23.8 | 1.3 | - | - |
| wrap | f | 137.0 | 14263.0 | 88.7 | 89.5 | 1.5 | calls fmodf | - |
| wrap | i | 7.3 | 1411.0 | 7.5 | 7.2 | 1.0 | - | - |
| wrap | x | 10.3 | 2050.0 | 10.5 | 10.2 | 1.0 | - | - |

ESP-DSP calibration: measured warm 1305.0 cycles/call; ANSI O2 reference 1311 cycles/call; measured/reference 99.5%.
<!-- /generated: math-benchmark -->
