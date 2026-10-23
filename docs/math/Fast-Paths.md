# Math fast paths

Fast paths pair a package implementation with straightforward C in the
[benchmark](Benchmark.md). Costs, bounds, build branches and callers come from
source annotations and committed captures. A dash means unmeasured.

<!-- generated: math-fast-paths check: python launcher/tools/perf/fast_paths.py --check -->
<a id="mathf-recip"></a>

## mathf_recip

**Plain way**

div_recip:

```c
1.0F / p->af.x
```

**What we do**

Reciprocal seed with two Newton corrections.

```math
y = y + y(1 - ay)
```

Implementation target: ESP32-S3 / Xtensa LX7.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `mathf_recip_f` | - | - | - | - |
| `div_recip_f` | - | - | - | - |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_RECIP_ULP_LIMIT` (1) | ulp | sweep |

Sweep: signed magnitudes and power-of-two edges.

Error curve: - (capture pending).

**Size**

Table bytes total the linked copies of each referenced table.

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `mathf_recip_f` | 62 / flash | - | - |
| `div_recip_f` | 45 / flash | `__divsf3`: - / ROM | - |

**Builds**

ELF instructions: `madd.s`, `msub.s`, `recip0.s`. Gating symbols: `CONFIG_MATH_FPU_RECIP`.

- `CONFIG_MATH_FPU_RECIP`: Use the ESP32-S3 FPU recip0.s and rsqrt0.s seeds. Disable where the emulator lacks these instructions (Espressif QEMU). QEMU: `n`.

Fast branch: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`.

Host (!defined(__XTENSA__)) compiles:

- `return 1.0F / a;`

QEMU compiles:

- `return 1.0F / a;`

**Where it pays**

| Engine module | Calls through | Max calls/frame | Median calls/frame |
|---|---|---:|---:|
| render | `mathf_recip`, `projectf_offset`, `vec3f_octahedral` | - | - |

<a id="mathf-rsqrt"></a>

## mathf_rsqrt

**Plain way**

div_rsqrt:

```c
1.0F / sqrtf(p->af.x)
```

**What we do**

Reciprocal-root seed with two Newton corrections.

```math
y = y + (y/2)(1 - vy^2)
```

Implementation target: ESP32-S3 / Xtensa LX7.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `mathf_rsqrt_f` | - | - | - | - |
| `div_rsqrt_f` | - | - | - | - |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_RSQRT_ULP_LIMIT` (1) | ulp | proof |

Proof: all mantissas in [1,4), normal exponent strides and edges.

Error curve: - (capture pending).

**Size**

Table bytes total the linked copies of each referenced table.

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `mathf_rsqrt_f` | 136 / flash | `__divsf3`: - / ROM, `sqrtf`: 16 / flash | - |
| `div_rsqrt_f` | 49 / flash | `__divsf3`: - / ROM, `sqrtf`: 16 / flash | - |

**Builds**

ELF instructions: `madd.s`, `msub.s`, `rsqrt0.s`. Gating symbols: `CONFIG_MATH_FPU_RECIP`.

- `CONFIG_MATH_FPU_RECIP`: Use the ESP32-S3 FPU recip0.s and rsqrt0.s seeds. Disable where the emulator lacks these instructions (Espressif QEMU). QEMU: `n`.

Fast branch: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`.

Host (!defined(__XTENSA__)) compiles:

- `return 1.0F / sqrtf(value);`

QEMU compiles:

- `return 1.0F / sqrtf(value);`

**Where it pays**

| Engine module | Calls through | Max calls/frame | Median calls/frame |
|---|---|---:|---:|
| anim | `quatf_nlerp`, `quatf_normalize`, `quatf_slerp` | - | - |
| apps | - | - | - |
| input | `vec3f_normalize` | - | - |
| math | `transformf_look_at`, `transformf_rotate_around` | - | - |
| scene | `transformf_from_mat` | - | - |

<a id="mathf-rsqrt-approx"></a>

## mathf_rsqrt_approx

**Plain way**

div_rsqrt:

```c
1.0F / sqrtf(p->af.x)
```

**What we do**

Reciprocal-root seed with one Newton correction.

```math
y = y(3/2 - vy^2/2)
```

Implementation target: ESP32-S3 / Xtensa LX7.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `mathf_rsqrt_approx_f` | - | - | - | - |
| `div_rsqrt_f` | - | - | - | - |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_RSQRT_APPROX_RELATIVE_LIMIT` (0.0001) | relative | proof |

Proof: all mantissas in [1,4), normal exponent strides and edges.

Error curve: - (capture pending).

**Size**

Table bytes total the linked copies of each referenced table.

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `mathf_rsqrt_approx_f` | 66 / flash | - | - |
| `div_rsqrt_f` | 49 / flash | `__divsf3`: - / ROM, `sqrtf`: 16 / flash | - |

**Builds**

ELF instructions: `msub.s`, `rsqrt0.s`. Gating symbols: `CONFIG_MATH_FPU_RECIP`.

- `CONFIG_MATH_FPU_RECIP`: Use the ESP32-S3 FPU recip0.s and rsqrt0.s seeds. Disable where the emulator lacks these instructions (Espressif QEMU). QEMU: `n`.

Fast branch: `defined(__XTENSA__) && CONFIG_MATH_FPU_RECIP`.

Host (!defined(__XTENSA__)) compiles:

- `return 1.0F / sqrtf(value);`

QEMU compiles:

- `return 1.0F / sqrtf(value);`

**Where it pays**

| Engine module | Calls through | Max calls/frame | Median calls/frame |
|---|---|---:|---:|
| render | `mathf_rsqrt_approx` | - | - |

<a id="mathx-rsqrt-q30"></a>

## mathx_rsqrt_q30

**Plain way**

normalize_div:

```c
normalize_div(p->ax)
```

nlerp_div:

```c
nlerp_div(p->qx, p->rx, MATH_BENCH_LERP_X)
```

**What we do**

Seed table, three integer Newton corrections and an exact midpoint bracket.

```math
y = round(2^{46}/sqrt(m))
```

Implementation target: Portable C.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `normalize_x` | - | - | - | - |
| `normalize_div_x` | - | - | - | - |
| `nlerp_x` | - | - | - | - |
| `nlerp_div_x` | - | - | - | - |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHX_RSQRT_Q30_RELATIVE_LIMIT` (4.65661e-10) | relative | proof |

Proof: mantissa segment strides and exact midpoint brackets.

Error curve: - (capture pending).

**Size**

Table bytes total the linked copies of each referenced table.

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `normalize_x` | 956 / flash | `__assert_func`: 236 / IRAM | `mathx_rsqrt_seeds`: 768 |
| `normalize_div_x` | 747 / flash | `__udivdi3`: 582 / ROM, `mathi_isqrt64_digits`: 1314 / flash | - |
| `nlerp_x` | 2664 / flash | `__assert_func`: 236 / IRAM | `mathx_rsqrt_seeds`: 768 |
| `nlerp_div_x` | 2220 / flash | `__divdi3`: 606 / ROM, `mathi_isqrt64_digits`: 1314 / flash | - |

**Builds**

ELF instructions: -. Gating symbols: -.

Fast branch: `all builds`.

Host and QEMU compile the same `mathx_rsqrt_q30` expression; no CONFIG_MATH_* branch selects it.

**Where it pays**

| Engine module | Calls through | Max calls/frame | Median calls/frame |
|---|---|---:|---:|
| - | - | - | - |

<a id="mathf-repeat"></a>

## mathf_repeat

**Plain way**

wrap_fmodf:

```c
wrap_fmodf(p->angle, -MATH_BENCH_WRAP_LIMIT, MATH_BENCH_WRAP_LIMIT)
```

**What we do**

Reciprocal quotient and fused remainder, with range fallback.

```math
r = fma(-floor(v/len), len, v)
```

Implementation target: ESP32-S3 / Xtensa LX7.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `wrap_f` | - | - | - | - |
| `wrap_fmodf_f` | - | - | - | - |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `MATHF_WRAP_ULP_LIMIT` (0) | ulp | sweep |

Sweep: signed float bit patterns, quotient guard and endpoint edges.

Error curve: - (capture pending).

**Size**

Table bytes total the linked copies of each referenced table.

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `wrap_f` | 270 / flash | `fmodf`: 342 / flash | - |
| `wrap_fmodf_f` | 118 / flash | `fmodf`: 342 / flash | - |

**Builds**

ELF instructions: `madd.s`, `msub.s`, `recip0.s`. Gating symbols: `CONFIG_MATH_FPU_FUSED_MADD`.

- `CONFIG_MATH_FPU_FUSED_MADD`: Rely on madd.s/msub.s rounding once. Disable where the emulator rounds the product (Espressif QEMU). QEMU: `n`.

Fast branch: `!defined(__XTENSA__) || CONFIG_MATH_FPU_FUSED_MADD`.

Host (!defined(__XTENSA__)) compiles:

- `return v;`

- `float rem = fmaf(-q, len, v);`

- `rem = fmaf(-(q - 1.0F), len, v);`

- `rem = fmaf(-(q + 1.0F), len, v);`

- `return 0.0F;`

- `return copysignf(0.0F, v);`

- `return rem < len ? rem : 0.0F;`

- `float r = fmodf(v, len);`

- `return r < len ? r : 0.0F;`

QEMU compiles:

- `return v;`

- `float r = fmodf(v, len);`

- `return r < len ? r : 0.0F;`

**Where it pays**

| Engine module | Calls through | Max calls/frame | Median calls/frame |
|---|---|---:|---:|
| - | - | - | - |

<a id="trig-sin"></a>

## trig_sin

**Plain way**

sinf:

```c
sinf((float)p->phase*(MATH_TAU / TRIG_TURN))
```

**What we do**

Quarter-wave table with linear interpolation.

```math
s = a + floor((b-a)t/256)
```

Implementation target: Portable C.

**Cost**

| Row | Measured target | Warm cycles/op | Cold cycles/call | Plain/fast warm |
|---|---|---:|---:|---:|
| `trig_sin_i` | - | - | - | - |
| `sinf_f` | - | - | - | - |

**Error**

| Measured target | Maximum error | Limit | Unit | Method |
|---|---:|---|---|---|
| - | - | `TRIG_SIN_LSB_LIMIT` (5) | lsb | sweep |

Sweep: every 16-bit phase against sinf in output LSBs.

Error curve: - (capture pending).

**Size**

Table bytes total the linked copies of each referenced table.

| Row | Code bytes / region | Callee bytes | Table bytes |
|---|---|---|---|
| `trig_sin_i` | 202 / flash | - | `trig_sin_quarter`: 1040 |
| `sinf_f` | 62 / flash | `_sinf`: 119 / flash | - |

**Builds**

ELF instructions: -. Gating symbols: -.

Fast branch: `all builds`.

Host and QEMU compile the same `trig_sin` expression; no CONFIG_MATH_* branch selects it.

**Where it pays**

| Engine module | Calls through | Max calls/frame | Median calls/frame |
|---|---|---:|---:|
| audio | `trig_sin` | - | - |
| boot | `trig_cos`, `trig_sin` | - | - |
| shell | `trig_cos`, `trig_sin` | - | - |
<!-- /generated: math-fast-paths -->
