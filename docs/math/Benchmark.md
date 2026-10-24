# Math benchmark

See [fast paths](Fast-Paths.md) for paired costs, error curves and callers.

Regenerate the benchmark and fast-path evidence with
`python launcher/tools/perf/fast_paths.py`; add `--check` to verify without writing.

The board supplies cycles and retired instructions. QEMU supplies instruction
counts for every row as information, without cache stalls or FPU latency.
Only float dot, normalize, mat4 mul and slerp are guarded by the CI ratchet.
The QEMU column marks wrappers containing `recip0.s`/`rsqrt0.s` as
"device-only instruction". `CONFIG_MATH_FPU_RECIP` gates the seed instructions;
the [build conditions](Fast-Paths.md) include the QEMU configuration.

Max error comes from `suite_math_numbers`; board and host captures stay separate.
`MATHERROR` rows report ulp steps from the rounded result; figures show ulp from the exact value.
Missing measurements show `-`.

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

## Capture

Capture the bench on the perf-scope diagnostics image and the error suite on
the plain diagnostics image:

```sh
autana --wait 3600 suite run_math_perf_suite --flash --perf-scope
autana --wait 3600 suite suite_math_numbers --flash
```

Keep the `BUILD_ID=`, `MATHTARGET`, `MATHBENCH`, `MATHERROR` and `MATHERRORBIN`
records in `data/math_bench_capture.txt`. Append `scope=bench` or `scope=error`
to each build ID line; exactly one of each is required for separate builds.
A capture from one image uses a single unscoped build ID.

`MATHTARGET` identifies the board shared by both captures.
Build the bench ELF locally with `autana build diag --perf-scope`.

Record wrapper metadata and instructions from the same build's ELF with
`python launcher/tools/perf/math_bench_table.py --from-elf <launcher.elf>`.
Record linked sizes with `python launcher/tools/perf/fast_paths.py --from-elf <launcher.elf>`.
The ELF manifest and capture carry row labels, types and op counts.

The fixed-point double-reference sweep runs with
`bash launcher/test/run_tests.sh --sanitize`. Keep its `MATHERROR` and
`MATHERRORBIN` records in `data/math_error_host_capture.txt`, with a
`MATHTARGET target=host arch=<architecture> execution=host` record.

Host evidence is labelled wherever it appears. Run
`python launcher/tools/perf/fast_paths.py` to update the evidence blocks, or
add `--check` to verify without writing. Generate the error plots with
`python launcher/tools/math/fast_path_figures.py`.

<details><summary>Full benchmark log</summary>

<!-- generated: math-benchmark check: python launcher/tools/perf/fast_paths.py --check -->
| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| add (vec3) | f | 15.3 | 1998.0 | 14.5 | 14.2 | 1.1 | - | - |
| add (vec3) | i | 14.3 | 1854.0 | 14.5 | 14.2 | 1.0 | - | - |
| add (vec3) | s | 37.4 | 4263.0 | 31.6 | 31.4 | 1.2 | - | - |
| add (vec3) | x | 58.4 | 6159.0 | 44.6 | 44.4 | 1.3 | - | - |
| atan2f | f | 336.7 | 29564.0 | 195.9 | 197.0 | 1.7 | calls atan2f | - |
| convert | f_to_i | 95.5 | 8862.0 | 62.1 | 62.0 | 1.5 | - | - |
| convert | f_to_s | 104.5 | 9927.0 | 75.6 | 75.5 | 1.4 | - | - |
| convert | f_to_x | 110.6 | 10044.0 | 70.7 | 70.5 | 1.6 | - | - |
| convert | i_to_f | 11.3 | 1725.0 | 11.5 | 11.2 | 1.0 | - | - |
| convert | s_to_f | 14.3 | 2148.0 | 14.5 | 14.3 | 1.0 | - | - |
| convert | x_to_f | 14.3 | 1908.0 | 14.5 | 14.3 | 1.0 | - | - |
| cosf | f | 191.5 | 21315.0 | 122.8 | 111.6 | 1.6 | calls _cosf | - |
| div_recip | f | 58.3 | 5104.0 | 40.5 | 40.3 | 1.4 | calls __divsf3 | - |
| div_rsqrt | f | 114.2 | 9841.0 | 81.5 | 82.3 | 1.4 | calls __divsf3, sqrtf | - |
| dot (vec3) | f | 17.4 | 2103.0 | 12.5 | 12.2 | 1.4 | madd.s | - |
| dot_f32_n256 | f | 1305.0 | 2370.0 | 1315.0 | 1299.0 | 1.0 | madd.s | - |
| dot (vec3) | i | 24.3 | 2733.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | s | 24.3 | 2910.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | x | 83.8 | 9596.0 | 65.9 | 65.8 | 1.3 | - | - |
| lerp (vec3) | f | 19.3 | 2466.0 | 17.5 | 17.3 | 1.1 | madd.s | - |
| lerp (vec3) | i | 101.3 | 9150.0 | 93.5 | 93.4 | 1.1 | - | - |
| lerp (vec3) | s | 33.3 | 3596.0 | 31.5 | 31.3 | 1.1 | - | - |
| lerp (vec3) | x | 127.3 | 12644.0 | 99.5 | 99.5 | 1.3 | - | - |
| mat4 mul | f | 738.4 | 49412.0 | 642.6 | 506.4 | 1.1 | madd.s; calls memcpy | - |
| mat4 mul | x | 2436.9 | 162409.0 | 1648.9 | 1640.4 | 1.5 | calls memcpy | - |
| mathf_recip | f | 26.3 | 3000.0 | 13.5 | device-only instruction | 1.9 | madd.s msub.s recip0.s | 1 step (sweep) |
| mathf_rsqrt_approx | f | 24.3 | 3084.0 | 12.5 | device-only instruction | 1.9 | msub.s rsqrt0.s | 5.94258e-05 relative (proof) |
| mathf_rsqrt | f | 38.4 | 4517.0 | 18.6 | device-only instruction | 2.1 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 step (proof) |
| mathx_rsqrt_q30 | x | 157.4 | 13717.0 | 143.6 | 143.6 | 1.1 | calls __assert_func | 4.61224169968e-10 relative (proof) (host) |
| mul (scalar) | f | 9.3 | 1477.0 | 6.5 | 6.2 | 1.4 | - | - |
| mul (scalar) | i | 9.3 | 1497.0 | 8.5 | 8.2 | 1.1 | - | - |
| mul (scalar) | s | 11.3 | 2451.0 | 10.5 | 10.2 | 1.1 | - | - |
| mul (scalar) | x | 22.3 | 3321.0 | 17.5 | 17.3 | 1.3 | - | - |
| nlerp_div | x | 738.4 | 61192.0 | 528.5 | 529.4 | 1.4 | calls __divdi3, mathi_isqrt64_digits | - |
| nlerp | f | 94.8 | 8989.0 | 53.7 | device-only instruction | 1.8 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 2 steps (sweep) |
| nlerp | x | 719.4 | 63286.0 | 558.8 | 909.9 | 1.3 | calls __assert_func | 4.61224169968e-10 relative (proof) (host) |
| normalize_div | x | 1114.7 | 77202.0 | 906.2 | 907.9 | 1.2 | calls __udivdi3, mathi_isqrt64_digits | - |
| normalize (vec3) | f | 57.4 | 5814.0 | 29.5 | device-only instruction | 1.9 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 step (sweep) |
| normalize (vec3) | x | 348.5 | 29326.0 | 296.9 | 681.2 | 1.2 | calls __assert_func | 4.61224169968e-10 relative (proof) (host) |
| quat mul | f | 30.3 | 3207.0 | 30.5 | 30.2 | 1.0 | madd.s msub.s | - |
| quat mul | x | 419.3 | 39758.0 | 313.5 | 313.9 | 1.3 | - | - |
| scale (vec3) | f | 13.3 | 1884.0 | 12.5 | 12.2 | 1.1 | - | - |
| scale (vec3) | i | 12.3 | 1882.0 | 12.5 | 12.2 | 1.0 | - | - |
| scale (vec3) | s | 36.4 | 4239.0 | 30.6 | 30.4 | 1.2 | - | - |
| scale (vec3) | x | 66.0 | 7671.0 | 49.2 | 49.0 | 1.3 | - | - |
| sinf | f | 223.7 | 23868.0 | 126.0 | 110.0 | 1.8 | calls _sinf | - |
| slerp | f | 452.7 | 34873.0 | 306.3 | device-only instruction | 1.5 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | - |
| sqrtf | f | 61.2 | 6170.0 | 45.5 | 46.3 | 1.3 | calls sqrtf | - |
| transform compute_matrix | f | 67.4 | 6781.0 | 67.6 | 67.4 | 1.0 | madd.s msub.s | - |
| transform compute_matrix | x | 855.7 | 74653.0 | 660.2 | 661.4 | 1.3 | calls mat4x_from_trs, memcpy | - |
| trig_sin | i | 30.8 | 4915.0 | 24.0 | 23.8 | 1.3 | - | 4.64258 lsb (sweep) |
| wrap | f | 63.8 | 7166.0 | 30.1 | device-only instruction | 2.1 | madd.s msub.s recip0.s; calls fmodf | 0 steps (sweep) |
| wrap_fmodf | f | 137.0 | 14131.0 | 88.7 | 88.6 | 1.5 | calls fmodf | - |
| wrap | i | 7.3 | 1350.0 | 7.5 | 7.2 | 1.0 | - | - |
| wrap | x | 10.3 | 1983.0 | 10.5 | 10.2 | 1.0 | - | - |

ESP-DSP calibration: measured warm 1305.0 cycles/call; ANSI O2 reference 1311 cycles/call; measured/reference 99.5%.
<!-- /generated: math-benchmark -->

</details>
