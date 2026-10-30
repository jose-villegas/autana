# simd

Integer lane operations on plain arrays. On the ESP32-S3 they run on the PIE
vector unit (128-bit registers of integer lanes); everywhere else, and in
QEMU, which stops at the first PIE instruction, the C references run. The
package depends on nothing, so every layer may use it. Pixel and audio
kernels that fuse several steps stay with their owners and build on this
package's frame macro and test pattern.

Include `"simd/<name>.h"`. Firmware declares `REQUIRES simd`; plain CMake
builds link the `simd` target.

## Calls

Every call has a `_ref` twin with the same signature: the C reference the
call matches bit for bit. A call takes any element-aligned pointer. It writes
the unaligned head and the tail itself and hands the aligned body to a
kernel; a run shorter than the call's minimum runs the reference alone, since
there the head and tail cost more than the vector body saves.

| Call | Does | Minimum |
|---|---|---|
| `simd_fill_u16(dst, value, count)` | `dst[i] = value` | `SIMD_FILL_U16_MIN` elements |
| `simd_repeat2_u16(dst, src, count)` | `dst[2i] = dst[2i + 1] = src[i]` | `SIMD_REPEAT2_U16_MIN` source elements |
| `simd_copy(dst, src, bytes)` | `memcpy` | `SIMD_COPY_MIN` bytes |

`simd_repeat2_u16` vectorises when `dst` is 4-byte aligned. A source of any
alignment is loaded with `EE.LD.128.USAR` and shifted into place.

## Rules

| Rule | Why |
|---|---|
| `CONFIG_SIMD_PIE` builds the kernels; `sdkconfig.defaults.qemu` turns it off | QEMU has no PIE instructions |
| Kernels load and store only at 16-byte boundaries | `EE.VLD.128` and `EE.VST.128` drop the low four address bits without a fault |
| No call from an ISR | PIE is coprocessor CP3, whose state is saved for tasks only |
| The first call pins an unpinned task to its core | ESP-IDF saves CP3 lazily in the owning task |
| A kernel opens its frame with `SIMD_PIE_FUNC` (`include/simd/pie_asm.h`) | The top 16 bytes of a windowed frame hold the caller's spilled registers |
| Kernels are `.S` leaves, never inline assembly | A kernel's `LOOPGTZ` owns the loop registers, which no caller keeps across a call |

Data in PSRAM gains little: the bus, not the arithmetic, limits a pass.

## Tests and timing

`tests/suite_simd.c` compares each call with its reference at every start
offset within a vector, at lengths around the minimum and at long runs, with
guard elements around the output. On the board it checks the PIE path;
elsewhere both sides are the reference. `bench/suite_simd_perf.c` (board,
perf scope) prints `SIMD_BENCH` rows: cycles of the reference and of the call
at each length.

## Headers

<!-- generated: package-simd-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/simd/copy.h](include/simd/copy.h) | copy: bytes from one array to another, memcpy's contract. |
| [include/simd/fill.h](include/simd/fill.h) | fill: one value written over an array. |
| [include/simd/pie_asm.h](include/simd/pie_asm.h) | pie_asm: the frame every PIE kernel (a .S file under src/pie/) opens and closes, so no kernel picks its own stack offsets. |
| [include/simd/repeat.h](include/simd/repeat.h) | repeat: every element of an array written more than once, side by side. |
| [include/simd/simd.h](include/simd/simd.h) | simd: what every call and kernel shares, the vector width and whether the PIE kernels are built. |
<!-- /generated: package-simd-headers -->
