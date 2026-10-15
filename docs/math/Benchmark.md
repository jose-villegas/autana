# Math benchmark

The board is the timing reference. QEMU counts executed instructions; it does
not model cache stalls, FPU latency or cycles. Only hot-path rows selected in
`suite_math_perf.c` participate in the CI instruction ratchet.
A row whose code differs between the device and QEMU builds stays out of the ratchet.

Warm is the median cycle-counter cost after an untimed kernel call, following
Embench-IoT's cache-warming method with fixed workloads.<sup>[[69]](../Citations.md#69)</sup>
Cold is the median of single calls after cache writeback and invalidation.
Inputs live in internal RAM, matching ESP-DSP's reference data placement.
The cache reset stalls the other core and suspends caches through IDF's flash
guard before invoking the S3 ROM whole-cache operations; cache restoration and
the call boundary precede timing. Board insn/op is one warmed batch counted
by the retired-instruction performance event on core zero, with frame counters
disarmed. Interrupts can contribute to measured samples.

An op is one input element, except `dot_f32_n256`: its op is one full dot
product call, matching ESP-DSP's published ANSI O2 workload for the S3. Its
published reference is 1311 cycles per call; compare this calibration row
against the ANSI cell.<sup>[[68]](../Citations.md#68)</sup> Compiler flags, loads, stores and call
overhead affect the comparison. Kernels retain all result components through
volatile stores, included in both the cycles and instruction columns.

Types use the [math suffixes](README.md#number-types); conversions name source
and destination. `mul (scalar)` is the per-type scalar multiply; `scale (vec3)`
multiplies a vector by a scalar. Transform compute_matrix composes TRS into a
fresh matrix, and quat mul composes rotations. Integer vectors have no
normalize, and fixed-point quaternions have no slerp.

Cycles/insn divides warm cycles by board instructions. FPU ops checks
`madd.s`, `msub.s`, `recip0.s`, `rsqrt0.s`, `div0.s`, `divn.s`, `sqrt0.s` directly in each device ELF wrapper and lists its call targets
as `calls` followed by callee symbols from the same disassembly; callee bodies
are not surveyed. An unresolved indirect target is shown as a starred register.
A `-` means neither the listed mnemonics nor calls occur in the wrapper.
QEMU baseline rows count instructions per wrapper call; the table divides by
the suite row's operation count to report insn/op. QEMU is `-` for table-only
rows and any row whose code differs between device and QEMU builds.

Record a board capture with `autana suite math_perf --flash`, retaining its
`BUILD_ID=` and `MATHBENCH` lines in `data/math_bench_capture.txt`. Record the
matching device ELF's wrapper instructions with
`python launcher/tools/perf/math_bench_table.py --from-elf launcher/build.diag/launcher.elf`.
Regenerate with `python launcher/tools/perf/math_bench_table.py > docs/math/Benchmark.md`.
Inputs are the suite's
row table, the checked-in capture and FPU data, and the instruction baseline.

<!-- generated: math-benchmark sha256=03b2555b6ee7f9ffd1e1ddf554306c76d8119821c98fbde69b64d6e71707ffd6 -->
Board capture pending; board columns remain unmeasured.

| function | type | cycles/op warm | cycles/op cold | board insn/op | QEMU insn/op | cycles/insn | FPU ops |
|---|---|---:|---:|---:|---:|---:|---|
| add (vec3) | f | - | - | - | - | - | - |
| scale (vec3) | f | - | - | - | - | - | - |
| dot (vec3) | f | - | - | - | 16.2 | - | madd.s |
| mul (scalar) | f | - | - | - | - | - | - |
| lerp (vec3) | f | - | - | - | - | - | madd.s |
| add (vec3) | i | - | - | - | - | - | - |
| scale (vec3) | i | - | - | - | - | - | - |
| dot (vec3) | i | - | - | - | - | - | - |
| mul (scalar) | i | - | - | - | - | - | - |
| lerp (vec3) | i | - | - | - | - | - | - |
| add (vec3) | s | - | - | - | - | - | - |
| scale (vec3) | s | - | - | - | - | - | - |
| dot (vec3) | s | - | - | - | - | - | - |
| mul (scalar) | s | - | - | - | - | - | - |
| lerp (vec3) | s | - | - | - | - | - | - |
| add (vec3) | x | - | - | - | - | - | - |
| scale (vec3) | x | - | - | - | - | - | - |
| dot (vec3) | x | - | - | - | - | - | - |
| mul (scalar) | x | - | - | - | - | - | - |
| lerp (vec3) | x | - | - | - | - | - | - |
| normalize (vec3) | f | - | - | - | 112.4 | - | madd.s; calls __divsf3, sqrtf |
| normalize (vec3) | x | - | - | - | - | - | calls __divdi3 |
| wrap | f | - | - | - | - | - | calls fmodf |
| wrap | i | - | - | - | - | - | - |
| wrap | x | - | - | - | - | - | - |
| convert | f_to_x | - | - | - | - | - | - |
| convert | x_to_f | - | - | - | - | - | - |
| convert | f_to_s | - | - | - | - | - | - |
| convert | s_to_f | - | - | - | - | - | - |
| convert | i_to_f | - | - | - | - | - | - |
| convert | f_to_i | - | - | - | - | - | - |
| slerp | f | - | - | - | 620.4 | - | madd.s; calls __divsf3, _sinf, sqrtf |
| nlerp | f | - | - | - | - | - | madd.s; calls __divsf3, sqrtf |
| nlerp | x | - | - | - | - | - | calls __divdi3 |
| quat mul | f | - | - | - | - | - | madd.s msub.s |
| quat mul | x | - | - | - | - | - | - |
| mat4 mul | f | - | - | - | 508.3 | - | madd.s; calls memcpy |
| mat4 mul | x | - | - | - | - | - | calls memcpy |
| transform compute_matrix | f | - | - | - | - | - | madd.s msub.s; calls memcpy |
| transform compute_matrix | x | - | - | - | - | - | calls mat4x_from_trs, memcpy |
| mathf_recip | f | - | - | - | - | - | madd.s msub.s recip0.s |
| mathf_rsqrt | f | - | - | - | - | - | msub.s rsqrt0.s |
| div_recip | f | - | - | - | - | - | calls __divsf3 |
| div_rsqrt | f | - | - | - | - | - | calls __divsf3, sqrtf |
| sinf | f | - | - | - | - | - | calls _sinf |
| cosf | f | - | - | - | - | - | calls _cosf |
| atan2f | f | - | - | - | - | - | calls atan2f |
| sqrtf | f | - | - | - | - | - | calls sqrtf |
| trig_sin | i | - | - | - | - | - | - |
| dot_f32_n256 | f | - | - | - | - | - | madd.s |
<!-- /generated: math-benchmark -->

<!-- generated: math-benchmark-inputs check: python launcher/tools/perf/math_bench_table.py --check -->
<!-- /generated: math-benchmark-inputs -->
