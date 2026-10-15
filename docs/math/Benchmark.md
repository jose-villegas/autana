# Math benchmark

The board supplies cycles and retired instructions. QEMU supplies instruction
counts, without cache stalls or FPU latency. Baseline membership selects the
instruction ratchet. Recip/rsqrt use `recip0.s`/`rsqrt0.s` on the device and a
portable path in QEMU, so their QEMU counts would guard code the board never runs.

Warm is the median after an untimed call, following Embench-IoT's cache-warming
method.<sup>[[69]](../Citations.md#69)</sup> Cold is the median per call after cache
writeback and invalidation, with both cores guarded through IDF's flash guard.
Inputs are in internal RAM, so data never misses and cold measures the code refill.
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

Record `BUILD_ID=` and `MATHBENCH` lines from `autana suite math_perf --flash`
in `data/math_bench_capture.txt`. Record wrapper metadata and instructions with
`python launcher/tools/perf/math_bench_table.py --from-elf launcher/build.diag/launcher.elf`.
The ELF manifest and capture carry row labels, types and op counts.
Run `python launcher/tools/perf/math_bench_table.py` to update the block, or add
`--check` to verify it without writing.

<!-- generated: math-benchmark check: python launcher/tools/perf/math_bench_table.py --check -->
Board capture pending; board columns remain unmeasured.

| function | type | cycles/op warm | cold cycles/call (I-cache refill) | board insn/op | QEMU insn/op | cycles/insn | fused/approx ops; calls |
|---|---|---:|---:|---:|---:|---:|---|
| add (vec3) | f | - | - | - | - | - | - |
| add (vec3) | i | - | - | - | - | - | - |
| add (vec3) | s | - | - | - | - | - | - |
| add (vec3) | x | - | - | - | - | - | - |
| atan2f | f | - | - | - | - | - | calls atan2f |
| convert | f_to_i | - | - | - | - | - | - |
| convert | f_to_s | - | - | - | - | - | - |
| convert | f_to_x | - | - | - | - | - | - |
| convert | i_to_f | - | - | - | - | - | - |
| convert | s_to_f | - | - | - | - | - | - |
| convert | x_to_f | - | - | - | - | - | - |
| cosf | f | - | - | - | - | - | calls _cosf |
| div_recip | f | - | - | - | - | - | calls __divsf3 |
| div_rsqrt | f | - | - | - | - | - | calls __divsf3, sqrtf |
| dot (vec3) | f | - | - | - | 12.2 | - | madd.s |
| dot_f32_n256 | f | - | - | - | - | - | madd.s |
| dot (vec3) | i | - | - | - | - | - | - |
| dot (vec3) | s | - | - | - | - | - | - |
| dot (vec3) | x | - | - | - | - | - | - |
| lerp (vec3) | f | - | - | - | - | - | madd.s |
| lerp (vec3) | i | - | - | - | - | - | - |
| lerp (vec3) | s | - | - | - | - | - | - |
| lerp (vec3) | x | - | - | - | - | - | - |
| mat4 mul | f | - | - | - | 506.4 | - | madd.s; calls memcpy |
| mat4 mul | x | - | - | - | - | - | divn.s; calls memcpy |
| mathf_recip | f | - | - | - | - | - | madd.s msub.s recip0.s |
| mathf_rsqrt | f | - | - | - | - | - | msub.s rsqrt0.s |
| mul (scalar) | f | - | - | - | - | - | - |
| mul (scalar) | i | - | - | - | - | - | - |
| mul (scalar) | s | - | - | - | - | - | - |
| mul (scalar) | x | - | - | - | - | - | - |
| nlerp | f | - | - | - | - | - | madd.s; calls __divsf3, sqrtf |
| nlerp | x | - | - | - | - | - | calls __divdi3 |
| normalize (vec3) | f | - | - | - | 100.4 | - | madd.s; calls __divsf3, sqrtf |
| normalize (vec3) | x | - | - | - | - | - | calls __divdi3 |
| quat mul | f | - | - | - | - | - | madd.s msub.s |
| quat mul | x | - | - | - | - | - | - |
| scale (vec3) | f | - | - | - | - | - | - |
| scale (vec3) | i | - | - | - | - | - | - |
| scale (vec3) | s | - | - | - | - | - | - |
| scale (vec3) | x | - | - | - | - | - | - |
| sinf | f | - | - | - | - | - | calls _sinf |
| slerp | f | - | - | - | 605.4 | - | madd.s; calls __divsf3, _sinf, sqrtf |
| sqrtf | f | - | - | - | - | - | calls sqrtf |
| transform compute_matrix | f | - | - | - | - | - | madd.s msub.s |
| transform compute_matrix | x | - | - | - | - | - | calls mat4x_from_trs, memcpy |
| trig_sin | i | - | - | - | - | - | - |
| wrap | f | - | - | - | - | - | calls fmodf |
| wrap | i | - | - | - | - | - | - |
| wrap | x | - | - | - | - | - | - |

ESP-DSP calibration: measured warm pending cycles/call; ANSI O2 reference 1311 cycles/call; measured/reference pending.
<!-- /generated: math-benchmark -->
