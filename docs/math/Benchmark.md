# Math benchmark

The board supplies cycles and retired instructions. QEMU supplies instruction
counts for every row as information, without cache stalls or FPU latency.
Only float dot, normalize, mat4 mul and slerp are guarded by the CI ratchet.
The QEMU column marks wrappers containing `recip0.s`/`rsqrt0.s` as
"device-only instruction": `CONFIG_LAUNCHER_QEMU` selects `1 / a` and
`1 / sqrtf(a)` instead of the device's seed instructions and Newton corrections.

Max error comes from the `suite_math_numbers` board run, appended to the capture.
Each `MATHERROR` line joins by table label and type; absent measurements show `-`.

`proof` covers every float in `[1, 4)`, each normal exponent's edges, and a
strided mantissa sample at every normal exponent, using an integer bracket.

`sweep` covers normalize and nlerp component errors against the `1 / sqrtf` path
across integer directions, magnitudes, quaternion pairs and blend weights.
For reciprocal it covers both signs over a geometric magnitude sweep and octave edges.

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
Board build: `e8baae1999ad-diag`.

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| add (vec3) | f | 15.3 | 2106.0 | 14.5 | 14.2 | 1.1 | - | - |
| add (vec3) | i | 14.3 | 2118.0 | 14.5 | 14.2 | 1.0 | - | - |
| add (vec3) | s | 37.4 | 5018.0 | 31.6 | 31.4 | 1.2 | - | - |
| add (vec3) | x | 58.4 | 6183.0 | 44.6 | 44.4 | 1.3 | - | - |
| atan2f | f | 336.7 | 29686.0 | 195.9 | 197.0 | 1.7 | calls atan2f | - |
| convert | f_to_i | 95.5 | 9039.0 | 62.1 | 62.0 | 1.5 | - | - |
| convert | f_to_s | 104.5 | 9641.0 | 75.6 | 75.5 | 1.4 | - | - |
| convert | f_to_x | 110.6 | 10246.0 | 70.7 | 70.5 | 1.6 | - | - |
| convert | i_to_f | 11.3 | 1824.0 | 11.5 | 11.2 | 1.0 | - | - |
| convert | s_to_f | 14.4 | 2043.0 | 14.5 | 14.3 | 1.0 | - | - |
| convert | x_to_f | 14.3 | 2280.0 | 14.5 | 14.3 | 1.0 | - | - |
| cosf | f | 191.5 | 21294.0 | 110.7 | 111.6 | 1.7 | calls _cosf | - |
| div_recip | f | 58.3 | 5339.0 | 204.6 | 40.3 | 0.3 | calls __divsf3 | - |
| div_rsqrt | f | 114.2 | 9787.0 | 81.5 | 82.3 | 1.4 | calls __divsf3, sqrtf | - |
| dot (vec3) | f | 17.3 | 2220.0 | 12.5 | 12.2 | 1.4 | madd.s | - |
| dot_f32_n256 | f | 1305.0 | 2448.0 | 1315.0 | 1299.0 | 1.0 | madd.s | - |
| dot (vec3) | i | 24.3 | 2741.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | s | 24.3 | 2987.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | x | 83.8 | 9459.0 | 65.9 | 65.8 | 1.3 | - | - |
| lerp (vec3) | f | 19.3 | 2483.0 | 17.5 | 17.3 | 1.1 | madd.s | - |
| lerp (vec3) | i | 101.3 | 9231.0 | 93.5 | 93.4 | 1.1 | - | - |
| lerp (vec3) | s | 33.3 | 3754.0 | 31.5 | 31.3 | 1.1 | - | - |
| lerp (vec3) | x | 127.4 | 12963.0 | 99.5 | 99.5 | 1.3 | - | - |
| mat4 mul | f | 738.4 | 49439.0 | 505.6 | 506.4 | 1.5 | madd.s; calls memcpy | - |
| mat4 mul | x | 2389.4 | 159548.0 | 1637.2 | 1640.4 | 1.5 | calls memcpy | - |
| mathf_recip | f | 26.3 | 3102.0 | 13.5 | device-only instruction | 1.9 | madd.s msub.s recip0.s | 1 ulp (sweep) |
| mathf_rsqrt_approx | f | 24.3 | 2993.0 | 12.5 | device-only instruction | 1.9 | msub.s rsqrt0.s | 5.94258e-05 relative (proof) |
| mathf_rsqrt | f | 38.4 | 4342.0 | 18.6 | device-only instruction | 2.1 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 ulp (proof) |
| mul (scalar) | f | 9.3 | 1441.0 | 6.5 | 6.2 | 1.4 | - | - |
| mul (scalar) | i | 9.3 | 1761.0 | 8.5 | 8.2 | 1.1 | - | - |
| mul (scalar) | s | 11.3 | 2287.0 | 10.5 | 10.2 | 1.1 | - | - |
| mul (scalar) | x | 22.3 | 3310.0 | 17.5 | 17.3 | 1.3 | - | - |
| nlerp | f | 94.8 | 8850.0 | 53.7 | device-only instruction | 1.8 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 2 ulp (sweep) |
| nlerp | x | 718.9 | 62715.0 | 558.8 | 909.9 | 1.3 | calls *a8, mu_pool_get+0xc, title_screen_layout+0x18 | - |
| normalize (vec3) | f | 57.4 | 5719.0 | 29.5 | device-only instruction | 1.9 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 ulp (sweep) |
| normalize (vec3) | x | 356.9 | 30067.0 | 319.9 | 681.2 | 1.1 | calls __assert_func | - |
| quat mul | f | 30.3 | 3303.0 | 30.5 | 30.2 | 1.0 | madd.s msub.s | - |
| quat mul | x | 419.3 | 38657.0 | 313.5 | 313.9 | 1.3 | - | - |
| scale (vec3) | f | 13.3 | 1977.0 | 12.5 | 12.2 | 1.1 | - | - |
| scale (vec3) | i | 12.3 | 1948.0 | 12.5 | 12.2 | 1.0 | - | - |
| scale (vec3) | s | 36.4 | 4341.0 | 30.6 | 30.4 | 1.2 | - | - |
| scale (vec3) | x | 66.0 | 7990.0 | 49.2 | 49.0 | 1.3 | - | - |
| sinf | f | 191.4 | 22213.0 | 109.1 | 110.0 | 1.8 | calls _sinf | - |
| slerp | f | 452.7 | 34760.0 | 306.3 | device-only instruction | 1.5 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | - |
| sqrtf | f | 61.2 | 5976.0 | 45.5 | 46.3 | 1.3 | calls sqrtf | - |
| transform compute_matrix | f | 67.4 | 6882.0 | 67.6 | 67.4 | 1.0 | madd.s msub.s | - |
| transform compute_matrix | x | 854.0 | 75037.0 | 660.2 | 661.4 | 1.3 | calls mat4x_from_trs, memcpy | - |
| trig_sin | i | 30.8 | 5082.0 | 24.0 | 23.8 | 1.3 | - | - |
| wrap | f | 137.0 | 14306.0 | 88.7 | 89.5 | 1.5 | calls fmodf | - |
| wrap | i | 7.3 | 1474.0 | 7.8 | 7.2 | 0.9 | - | - |
| wrap | x | 10.3 | 2022.0 | 10.5 | 10.2 | 1.0 | - | - |

ESP-DSP calibration: measured warm 1305.0 cycles/call; ANSI O2 reference 1311 cycles/call; measured/reference 99.5%.
<!-- /generated: math-benchmark -->
