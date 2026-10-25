# Math benchmark

Board cycles and instructions per op for every bench row, with QEMU instruction counts for information.
See [fast paths](Fast-Paths.md) for each fast path's plain-versus-ours cost, error and target.

## How it is measured

| what | how | caveat |
|---|---|---|
| Warm | Median after an untimed call, following Embench-IoT's cache-warming method.<sup>[[69]](../Citations.md#69)</sup> | Includes call overhead. |
| Cold | Median per call after `esp_cache_msync()` invalidates I-cache and flash-rodata D-cache over linker-defined, cache-line-rounded ranges, without writeback. | Includes libm and other callees; caches stay enabled, PSRAM untouched. Inputs and outputs are in internal RAM; calls refill code and flash rodata. |
| Board instructions | Count a warmed call on core zero with frame counters disarmed; a compiler barrier preserves every output component. | Interrupts can contribute. |
| QEMU instructions | Wrapper instructions divided by the row's op count. | No cache stalls or FPU latency. `recip0.s`/`rsqrt0.s`, gated by `CONFIG_MATH_FPU_RECIP`, show as "device-only instruction". CI ratchets only float dot, normalize, mat4 mul and slerp. |
| Cycles/insn | Warm cycles divided by board instructions. | Includes stalls and call overhead. |
| Op | One input element; types follow [math suffixes](README.md#number-types), conversions name source and destination. | Mat4 includes by-value struct copies; transform compute_matrix composes TRS, quat mul composes rotations. |
| Fused/approx column | Matching wrapper mnemonics and sorted callees. | Callee bodies are excluded; unresolved indirect targets are starred registers; `-` means no match. |
| Max error | Measured by `suite_math_numbers` on the board; host captures are labelled separately. Values count ulp steps from the rounded result. | A fast path's error is in its linked Fast-Paths section. Only rows without a section carry a value here; `-` means no measurement. |
| Calibration | `dot_f32_n256` is a full dot-product call matching ESP-DSP's ANSI O2 workload.<sup>[[68]](../Citations.md#68)</sup> | Compares warm cycles/call; compiler flags, loads, stores and call overhead affect the ratio. |

![Warm benchmark costs grouped by math family and number type](../images/math/benchmark-families.png)

Warm cycles per op, generated from the committed capture.

<details><summary>Full benchmark log</summary>

<!-- generated: math-benchmark check: python launcher/tools/perf/fast_paths.py --check -->
#### Vector

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| add (vec3) | f | 15.3 | 1998.0 | 14.5 | 14.2 | 1.1 | - | - |
| add (vec3) | i | 14.3 | 1854.0 | 14.5 | 14.2 | 1.0 | - | - |
| add (vec3) | s | 37.4 | 4263.0 | 31.6 | 31.4 | 1.2 | - | - |
| add (vec3) | x | 58.4 | 6159.0 | 44.6 | 44.4 | 1.3 | - | - |
| convert | f_to_i | 95.5 | 8862.0 | 62.1 | 62.0 | 1.5 | - | - |
| convert | f_to_s | 104.5 | 9927.0 | 75.6 | 75.5 | 1.4 | - | - |
| convert | f_to_x | 110.6 | 10044.0 | 70.7 | 70.5 | 1.6 | - | - |
| convert | i_to_f | 11.3 | 1725.0 | 11.5 | 11.2 | 1.0 | - | - |
| convert | s_to_f | 14.3 | 2148.0 | 14.5 | 14.3 | 1.0 | - | - |
| convert | x_to_f | 14.3 | 1908.0 | 14.5 | 14.3 | 1.0 | - | - |
| dot (vec3) | f | 17.4 | 2103.0 | 12.5 | 12.2 | 1.4 | madd.s | - |
| dot (vec3) | i | 24.3 | 2733.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | s | 24.3 | 2910.0 | 24.5 | 24.2 | 1.0 | - | - |
| dot (vec3) | x | 83.8 | 9596.0 | 65.9 | 65.8 | 1.3 | - | - |
| lerp (vec3) | f | 19.3 | 2466.0 | 17.5 | 17.3 | 1.1 | madd.s | - |
| lerp (vec3) | i | 101.3 | 9150.0 | 93.5 | 93.4 | 1.1 | - | - |
| lerp (vec3) | s | 33.3 | 3596.0 | 31.5 | 31.3 | 1.1 | - | - |
| lerp (vec3) | x | 127.3 | 12644.0 | 99.5 | 99.5 | 1.3 | - | - |
| [normalize_div](Fast-Paths.md#mathx-rsqrt-q30) | x | 1114.7 | 77202.0 | 906.2 | 907.9 | 1.2 | calls __udivdi3, mathi_isqrt64_digits | - |
| normalize (vec3) | f | 57.4 | 5814.0 | 29.5 | device-only instruction | 1.9 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 1 step (sweep) |
| [normalize (vec3)](Fast-Paths.md#mathx-rsqrt-q30) | x | 348.5 | 29326.0 | 296.9 | 681.2 | 1.2 | calls __assert_func | [fast paths](Fast-Paths.md#mathx-rsqrt-q30) |
| scale (vec3) | f | 13.3 | 1884.0 | 12.5 | 12.2 | 1.1 | - | - |
| scale (vec3) | i | 12.3 | 1882.0 | 12.5 | 12.2 | 1.0 | - | - |
| scale (vec3) | s | 36.4 | 4239.0 | 30.6 | 30.4 | 1.2 | - | - |
| scale (vec3) | x | 66.0 | 7671.0 | 49.2 | 49.0 | 1.3 | - | - |

#### Scalar

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| [div_recip](Fast-Paths.md#mathf-recip) | f | 58.3 | 5104.0 | 40.5 | 40.3 | 1.4 | calls __divsf3 | - |
| [div_rsqrt](Fast-Paths.md#mathf-rsqrt) | f | 114.2 | 9841.0 | 81.5 | 82.3 | 1.4 | calls __divsf3, sqrtf | - |
| [mathf_recip](Fast-Paths.md#mathf-recip) | f | 26.3 | 3000.0 | 13.5 | device-only instruction | 1.9 | madd.s msub.s recip0.s | [fast paths](Fast-Paths.md#mathf-recip) |
| [mathf_rsqrt_approx](Fast-Paths.md#mathf-rsqrt-approx) | f | 24.3 | 3084.0 | 12.5 | device-only instruction | 1.9 | msub.s rsqrt0.s | [fast paths](Fast-Paths.md#mathf-rsqrt-approx) |
| [mathf_rsqrt](Fast-Paths.md#mathf-rsqrt) | f | 38.4 | 4517.0 | 18.6 | device-only instruction | 2.1 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | [fast paths](Fast-Paths.md#mathf-rsqrt) |
| [mathx_rsqrt_q30](Fast-Paths.md#mathx-rsqrt-q30) | x | 157.4 | 13717.0 | 143.6 | 143.6 | 1.1 | calls __assert_func | [fast paths](Fast-Paths.md#mathx-rsqrt-q30) |
| mul (scalar) | f | 9.3 | 1477.0 | 6.5 | 6.2 | 1.4 | - | - |
| mul (scalar) | i | 9.3 | 1497.0 | 8.5 | 8.2 | 1.1 | - | - |
| mul (scalar) | s | 11.3 | 2451.0 | 10.5 | 10.2 | 1.1 | - | - |
| mul (scalar) | x | 22.3 | 3321.0 | 17.5 | 17.3 | 1.3 | - | - |
| [trig_sin](Fast-Paths.md#trig-sin) | i | 30.8 | 4915.0 | 24.0 | 23.8 | 1.3 | - | [fast paths](Fast-Paths.md#trig-sin) |
| [wrap](Fast-Paths.md#mathf-repeat) | f | 63.8 | 7166.0 | 30.1 | device-only instruction | 2.1 | madd.s msub.s recip0.s; calls fmodf | [fast paths](Fast-Paths.md#mathf-repeat) |
| wrap | i | 7.3 | 1350.0 | 7.5 | 7.2 | 1.0 | - | - |
| wrap | x | 10.3 | 1983.0 | 10.5 | 10.2 | 1.0 | - | - |

#### Quaternion

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| [nlerp_div](Fast-Paths.md#mathx-rsqrt-q30) | x | 738.4 | 61192.0 | 528.5 | 529.4 | 1.4 | calls __divdi3, mathi_isqrt64_digits | - |
| nlerp | f | 94.8 | 8989.0 | 53.7 | device-only instruction | 1.8 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | 2 steps (sweep) |
| [nlerp](Fast-Paths.md#mathx-rsqrt-q30) | x | 719.4 | 63286.0 | 558.8 | 909.9 | 1.3 | calls __assert_func | [fast paths](Fast-Paths.md#mathx-rsqrt-q30) |
| quat mul | f | 30.3 | 3207.0 | 30.5 | 30.2 | 1.0 | madd.s msub.s | - |
| quat mul | x | 419.3 | 39758.0 | 313.5 | 313.9 | 1.3 | - | - |
| slerp | f | 452.7 | 34873.0 | 306.3 | device-only instruction | 1.5 | madd.s msub.s rsqrt0.s; calls __divsf3, sqrtf | - |

#### Matrix

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| mat4 mul | f | 738.4 | 49412.0 | 642.6 | 506.4 | 1.1 | madd.s; calls memcpy | - |
| mat4 mul | x | 2436.9 | 162409.0 | 1648.9 | 1640.4 | 1.5 | calls memcpy | - |
| transform compute_matrix | f | 67.4 | 6781.0 | 67.6 | 67.4 | 1.0 | madd.s msub.s | - |
| transform compute_matrix | x | 855.7 | 74653.0 | 660.2 | 661.4 | 1.3 | calls mat4x_from_trs, memcpy | - |

#### Libm

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| atan2f | f | 336.7 | 29564.0 | 195.9 | 197.0 | 1.7 | calls atan2f | - |
| cosf | f | 191.5 | 21315.0 | 122.8 | 111.6 | 1.6 | calls _cosf | - |
| [sinf](Fast-Paths.md#trig-sin) | f | 223.7 | 23868.0 | 126.0 | 110.0 | 1.8 | calls _sinf | - |
| sqrtf | f | 61.2 | 6170.0 | 45.5 | 46.3 | 1.3 | calls sqrtf | - |
| [wrap_fmodf](Fast-Paths.md#mathf-repeat) | f | 137.0 | 14131.0 | 88.7 | 88.6 | 1.5 | calls fmodf | - |

#### Calibration

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls | max error |
|---|---|---:|---:|---:|---:|---:|---|---|
| dot_f32_n256 | f | 1305.0 | 2370.0 | 1315.0 | 1299.0 | 1.0 | madd.s | - |

ESP-DSP calibration: measured warm 1305.0 cycles/call; ANSI O2 reference 1311 cycles/call; measured/reference 99.5%.
<!-- /generated: math-benchmark -->

</details>

## Capture

| step | command |
|---|---|
| Bench capture | `idf.py -C launcher/packages/math/test_apps set-target esp32s3 flash`, then `idf.py -C launcher/packages/math/test_apps monitor > data/math_bench_capture.txt` |
| Error capture | Included in the package app capture |
| ELF metadata | `idf.py -C launcher/packages/math/test_apps build`, then `python launcher/tools/perf/math_bench_table.py --from-elf <math_test_app.elf>` |
| Linked sizes | `python launcher/tools/perf/fast_paths.py --from-elf <math_test_app.elf>` |
| Host error sweep | `bash launcher/test/run_tests.sh --sanitize` |
| Host records | Save `MATHERROR`/`MATHERRORBIN` in `data/math_error_host_capture.txt` with a `MATHTARGET target=host arch=<architecture> execution=host` record. |
| Regenerate blocks | `python launcher/tools/perf/fast_paths.py`; add `--check` to verify. |
| Figures | `bash launcher/tools/render/render_doc_images.sh`, or `python launcher/tools/math/math_bench_figures.py` and `python launcher/tools/math/fast_path_figures.py`. |

The package app ELF is `launcher/packages/math/test_apps/build/math_test_app.elf`.

Keep `BUILD_ID=`, `MATHTARGET`, `MATHBENCH`, `MATHERROR` and `MATHERRORBIN` records in `data/math_bench_capture.txt`.
Separate images require one `scope=bench` and one `scope=error` build ID; a single image uses one unscoped ID.

`MATHTARGET` identifies the shared target; ELF metadata and sizes must match the bench image. Host evidence is labelled separately.
