# Math fast paths

Each section compares plain C with the package implementation: algorithm,
measured cost and error, linked size, build conditions, and calling modules.
The [capture commands](Benchmark.md#capture) produce the evidence.

Key: warm cycles measure cached batches; cold cycles include instruction-cache refill.
Plain/fast warm is plain ÷ fast; greater than one means the fast path wins.
Regions are ELF flash, IRAM, read-only data, or absolute ROM symbols; tables total linked copies.
A dash means unmeasured. Package wrapper modules are included. App callers share the row “apps (not named)”.

<!-- generated: math-fast-paths check: python launcher/tools/perf/fast_paths.py --check -->
<a id="mathf-recip"></a>

## mathf_recip

**Plain way**

1.0F / x.

**What we do**

For nonzero input a, reciprocal seed y with two Newton corrections.

```math
y = y + y(1 - ay)
```

Implementation target: Target-specific.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `mathf_recip_f` | - | - | - | - |
| `div_recip_f` | - | - | - | n/a |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_RECIP_ULP_LIMIT` (1) | ulp | sweep |

Sweep: signed magnitudes and power-of-two edges.

Error curve: - (capture pending).

**Size**

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `mathf_recip_f` | 62 / flash | none | 0 |
| `div_recip_f` | 45 / flash | `__divsf3`: - / ROM | 0 |

**Builds**

ELF instructions: `madd.s`, `msub.s`, `recip0.s`. Gating symbols: `CONFIG_MATH_FPU_RECIP`.

- `CONFIG_MATH_FPU_RECIP`: Use the ESP32-S3 FPU recip0.s and rsqrt0.s seeds. Disable where the emulator lacks these instructions (Espressif QEMU). QEMU: `n`.

Host: `!defined(__XTENSA__)`; source conditions: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`.

QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP` (values above).

Target: source conditions `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`; configured values come from the capture build.

**Where it pays**

| Engine module | Calls through |
|---|---|
| math/linear | `mathf_recip`, `projectf_offset` |
| math/scalar | `mathf_recip`, `mathf_repeat` |
| render | `mathf_recip`, `projectf_offset`, `vec3f_octahedral` |

<a id="mathf-rsqrt"></a>

## mathf_rsqrt

**Plain way**

1.0F / sqrtf(x).

**What we do**

For positive input v, reciprocal-root seed y with two Newton corrections.

```math
y = y + (y/2)(1 - vy^2)
```

Implementation target: Target-specific.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `mathf_rsqrt_f` | - | - | - | - |
| `div_rsqrt_f` | - | - | - | n/a |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_RSQRT_ULP_LIMIT` (1) | ulp | proof |

Proof: all mantissas in [1,4), normal exponent strides and edges.

Error curve: - (capture pending).

**Size**

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `mathf_rsqrt_f` | 136 / flash | `__divsf3`: - / ROM, `sqrtf`: 16 / flash | 0 |
| `div_rsqrt_f` | 49 / flash | `__divsf3`: - / ROM, `sqrtf`: 16 / flash | 0 |

**Builds**

ELF instructions: `madd.s`, `msub.s`, `rsqrt0.s`. Gating symbols: `CONFIG_MATH_FPU_RECIP`.

- `CONFIG_MATH_FPU_RECIP`: Use the ESP32-S3 FPU recip0.s and rsqrt0.s seeds. Disable where the emulator lacks these instructions (Espressif QEMU). QEMU: `n`.

Host: `!defined(__XTENSA__)`; source conditions: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`.

QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP` (values above).

Target: source conditions `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`; configured values come from the capture build.

**Where it pays**

| Engine module | Calls through |
|---|---|
| anim | `quatf_nlerp`, `quatf_normalize`, `quatf_slerp` |
| apps (not named) | `vec3f_normalize` |
| input | `vec3f_normalize` |
| math | `transformf_look_at`, `transformf_rotate_around` |
| math/linear | `quatf_nlerp`, `quatf_normalize`, `vec3f_normalize` |
| scene | `transformf_from_mat` |

<a id="mathf-rsqrt-approx"></a>

## mathf_rsqrt_approx

**Plain way**

1.0F / sqrtf(x).

**What we do**

For positive input v, reciprocal-root seed y with one Newton correction.

```math
y = y(3/2 - (v/2)y^2)
```

Implementation target: Target-specific.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `mathf_rsqrt_approx_f` | - | - | - | - |
| `div_rsqrt_f` | - | - | - | n/a |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_RSQRT_APPROX_RELATIVE_LIMIT` (0.0001) | relative | proof |

Proof: all mantissas in [1,4), normal exponent strides and edges.

Error curve: - (capture pending).

**Size**

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `mathf_rsqrt_approx_f` | 66 / flash | none | 0 |
| `div_rsqrt_f` | 49 / flash | `__divsf3`: - / ROM, `sqrtf`: 16 / flash | 0 |

**Builds**

ELF instructions: `msub.s`, `rsqrt0.s`. Gating symbols: `CONFIG_MATH_FPU_RECIP`.

- `CONFIG_MATH_FPU_RECIP`: Use the ESP32-S3 FPU recip0.s and rsqrt0.s seeds. Disable where the emulator lacks these instructions (Espressif QEMU). QEMU: `n`.

Host: `!defined(__XTENSA__)`; source conditions: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`.

QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP` (values above).

Target: source conditions `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`; configured values come from the capture build.

**Where it pays**

| Engine module | Calls through |
|---|---|
| render | `mathf_rsqrt_approx` |

<a id="mathx-rsqrt-q30"></a>

## mathx_rsqrt_q30

**Plain way**

isqrt64 squared length, then 64-bit divide and component scaling.

linear quaternion blend, isqrt64 length, then 64-bit divide normalization.

**What we do**

For integer mantissa m in [MATHX_RSQRT_MIN, 2^32), seed table, three Newton corrections and exact midpoint bracket give reciprocal root y.

```math
y = round(2^{46}/sqrt(m))
```

Implementation target: Portable C.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `normalize_x` | - | - | - | - |
| `normalize_div_x` | - | - | - | n/a |
| `nlerp_x` | - | - | - | - |
| `nlerp_div_x` | - | - | - | n/a |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHX_RSQRT_Q30_RELATIVE_LIMIT` (4.65661e-10) | relative | proof |

Proof: mantissa segment strides and exact midpoint brackets.

Error curve: - (capture pending).

**Size**

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `normalize_x` | 956 / flash | `__assert_func`: 236 / IRAM | `mathx_rsqrt_seeds`: 768 |
| `normalize_div_x` | 747 / flash | `__udivdi3`: 582 / ROM, `mathi_isqrt64_digits`: 1314 / flash | 0 |
| `nlerp_x` | 2664 / flash | `__assert_func`: 236 / IRAM | `mathx_rsqrt_seeds`: 768 |
| `nlerp_div_x` | 2220 / flash | `__divdi3`: 606 / ROM, `mathi_isqrt64_digits`: 1314 / flash | 0 |

**Builds**

ELF instructions: none. Gating symbols: none.

Host: `!defined(__XTENSA__)`; source conditions: `all builds (portable C)`.

QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `all builds (portable C)` (values above).

Target: source conditions `all builds (portable C)`; configured values come from the capture build.

**Where it pays**

| Engine module | Calls through |
|---|---|
| math/linear | `mathx_normalize_n`, `quatx_unit_nlerp`, `vec3x_normalize_divide` |
| math/scalar | `mathx_rsqrt_q30` |

<a id="mathf-repeat"></a>

## mathf_repeat

**Plain way**

fmodf(v - lo, hi - lo), adjusted into [lo, hi).

**What we do**

For input v and positive period len, reciprocal quotient and fused remainder r, with range fallback.

```math
r = fma(-floor(v/len), len, v)
```

Implementation target: Portable C.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `wrap_f` | - | - | - | - |
| `wrap_fmodf_f` | - | - | - | n/a |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_WRAP_ULP_LIMIT` (0) | ulp | sweep |

Sweep: signed float bit patterns, quotient guard and endpoint edges.

Error curve: - (capture pending).

**Size**

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `wrap_f` | 270 / flash | `fmodf`: 342 / flash | 0 |
| `wrap_fmodf_f` | 118 / flash | `fmodf`: 342 / flash | 0 |

**Builds**

ELF instructions: `madd.s`, `msub.s`, `recip0.s`. Gating symbols: `CONFIG_MATH_FPU_FUSED_MADD`.

- `CONFIG_MATH_FPU_FUSED_MADD`: Rely on madd.s/msub.s rounding once. Disable where the emulator rounds the product (Espressif QEMU). QEMU: `n`.

Host: `!defined(__XTENSA__)`; source conditions: `!defined(__XTENSA__) || CONFIG_MATH_FPU_FUSED_MADD`.

QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `!defined(__XTENSA__) || CONFIG_MATH_FPU_FUSED_MADD` (values above).

Target: source conditions `!defined(__XTENSA__) || CONFIG_MATH_FPU_FUSED_MADD`; configured values come from the capture build.

**Where it pays**

| Engine module | Calls through |
|---|---|
| math/scalar | `mathf_repeat` |

<a id="trig-sin"></a>

## trig_sin

**Plain way**

sinf(phase * (MATH_TAU / TRIG_TURN)).

**What we do**

For 16-bit phase input, quarter-wave samples a and b bracket it; t is the low eight phase bits and s is sine in output LSBs.

```math
s = a + floor((b-a)t/256)
```

Implementation target: Portable C.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `trig_sin_i` | - | - | - | - |
| `sinf_f` | - | - | - | n/a |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `TRIG_SIN_LSB_LIMIT` (5) | lsb | sweep |

Sweep: every 16-bit phase against sinf in output LSBs.

Error curve: - (capture pending).

**Size**

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `trig_sin_i` | 202 / flash | none | `trig_sin_quarter`: 1040 |
| `sinf_f` | 62 / flash | `_sinf`: 119 / flash | 0 |

**Builds**

ELF instructions: none. Gating symbols: none.

Host: `!defined(__XTENSA__)`; source conditions: `all builds (portable C)`.

QEMU: `CONFIG_LAUNCHER_QEMU`; source conditions: `all builds (portable C)` (values above).

Target: source conditions `all builds (portable C)`; configured values come from the capture build.

**Where it pays**

| Engine module | Calls through |
|---|---|
| audio | `trig_sin` |
| boot | `trig_cos`, `trig_sin` |
| math/scalar | `mathx_cos_turns`, `mathx_sin_turns`, `trig_cos`, `trig_sin` |
| shell | `trig_cos`, `trig_sin` |
<!-- /generated: math-fast-paths -->
