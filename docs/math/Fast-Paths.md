# Math fast paths

The tables compare plain C with the package implementation: algorithm,
measured cost and error, linked size, build conditions, and calling modules.
The [capture commands](Benchmark.md#capture) produce the evidence.

Key: warm cycles measure cached batches; cold cycles include instruction-cache refill.
Plain/fast is the warm cycle ratio (greater than one wins).
Code totals wrappers and callees by ELF region; table bytes total linked copies.
A dash means no measurement or table. Callers are engine modules; app names are grouped.

<!-- generated: math-fast-paths check: python launcher/tools/perf/fast_paths.py --check -->
Measured target: esp32s3 (Xtensa, LX7_ESP32_S3_MP).

### What it replaces

| Fast path | Plain way | Ours | Target / hardware feature |
|---|---|---|---|
| [`mathf_recip`](#mathf-recip) | `1.0F / x` | reciprocal seed with two Newton corrections | Xtensa: `recip0.s`, `madd.s`, `msub.s` |
| [`mathf_rsqrt`](#mathf-rsqrt) | `1.0F / sqrtf(x)` | reciprocal-root seed with two Newton corrections | Xtensa: `rsqrt0.s`, `madd.s`, `msub.s` |
| [`mathf_rsqrt_approx`](#mathf-rsqrt-approx) | `1.0F / sqrtf(x)` | reciprocal-root seed with one Newton correction | Xtensa: `rsqrt0.s`, `msub.s` |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | isqrt64 squared length, then 64-bit divide and component scaling | seed table, three Newton corrections and exact midpoint bracket | portable |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | linear quaternion blend, isqrt64 length, then 64-bit divide normalization | seed table, three Newton corrections and exact midpoint bracket | portable |
| [`mathf_repeat`](#mathf-repeat) | `fmodf(v - lo, hi - lo)` | reciprocal quotient and fused remainder, with range fallback | portable; Xtensa needs fused `madd.s`, `msub.s` |
| [`trig_sin`](#trig-sin) | `sinf(phase * (MATH_TAU / TRIG_TURN))` | quarter-wave interpolation | portable |

### Cost and error

| Fast path | Bench rows | Warm cycles fast | Warm plain | Plain/fast | Cold fast | Cold plain | Max error | Limit | Method |
|---|---|---|---|---|---|---|---|---|---|
| [`mathf_recip`](#mathf-recip) | `mathf_recip_f` vs `div_recip_f` | 26.3 | 58.3 | 2.22 | 3000.0 | 5104.0 | 1 ulp | `MATHF_RECIP_ULP_LIMIT` (1) | sweep |
| [`mathf_rsqrt`](#mathf-rsqrt) | `mathf_rsqrt_f` vs `div_rsqrt_f` | 38.4 | 114.2 | 2.97 | 4517.0 | 9841.0 | 1 ulp | `MATHF_RSQRT_ULP_LIMIT` (1) | proof |
| [`mathf_rsqrt_approx`](#mathf-rsqrt-approx) | `mathf_rsqrt_approx_f` vs `div_rsqrt_f` | 24.3 | 114.2 | 4.70 | 3084.0 | 9841.0 | 5.94258e-05 relative | `MATHF_RSQRT_APPROX_RELATIVE_LIMIT` (0.0001) | proof |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | `normalize_x` vs `normalize_div_x` | 348.5 | 1114.7 | 3.20 | 29326.0 | 77202.0 | 4.61224169968e-10 relative (host) | `MATHX_RSQRT_Q30_RELATIVE_LIMIT` (2^-31) | proof |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | `nlerp_x` vs `nlerp_div_x` | 719.4 | 738.4 | 1.03 | 63286.0 | 61192.0 | 4.61224169968e-10 relative (host) | `MATHX_RSQRT_Q30_RELATIVE_LIMIT` (2^-31) | proof |
| [`mathf_repeat`](#mathf-repeat) | `wrap_f` vs `wrap_fmodf_f` | 63.8 | 137.0 | 2.15 | 7166.0 | 14131.0 | 0 ulp | `MATHF_WRAP_ULP_LIMIT` (0) | sweep |
| [`trig_sin`](#trig-sin) | `trig_sin_i` vs `sinf_f` | 30.8 | 223.7 | 7.26 | 4915.0 | 23868.0 | 4.64258 lsb | `TRIG_SIN_LSB_LIMIT` (5) | sweep |

### Size and builds

| Fast path | Fast code bytes (region) | Plain code bytes (region) | Table bytes | Gating Kconfig | QEMU | Host | Callers |
|---|---|---|---|---|---|---|---|
| [`mathf_recip`](#mathf-recip) | 62 flash | ROM + 45 flash | - | `CONFIG_MATH_FPU_RECIP` | plain | plain | `render` |
| [`mathf_rsqrt`](#mathf-rsqrt) | ROM + 152 flash | ROM + 65 flash | - | `CONFIG_MATH_FPU_RECIP` | plain | plain | `anim`, `apps (not named)`, `input`, `scene` |
| [`mathf_rsqrt_approx`](#mathf-rsqrt-approx) | 66 flash | ROM + 65 flash | - | `CONFIG_MATH_FPU_RECIP` | plain | plain | `render` |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | 236 IRAM + 956 flash | ROM + 1666 flash | 192 | none | fast | fast | none |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | 236 IRAM + 2664 flash | ROM + 3139 flash | 192 | none | fast | fast | none |
| [`mathf_repeat`](#mathf-repeat) | 612 flash | 460 flash | - | `CONFIG_MATH_FPU_FUSED_MADD` | plain | fast | none |
| [`trig_sin`](#trig-sin) | 202 flash | 181 flash | 520 | none | fast | fast | `audio`, `boot`, `shell` |

<a id="mathf-recip"></a>

## `mathf_recip`

```math
y = y + y(1 - ay)
```

Symbols: nonzero input `a`; reciprocal seed `y`.

Sweep: signed magnitudes and power-of-two edges.

![Error across the input range](../images/math/fast-path-mathf_recip.png)

<a id="mathf-rsqrt"></a>

## `mathf_rsqrt`

```math
y = y + (y/2)(1 - vy^2)
```

Symbols: positive input `v`; reciprocal-root seed `y`.

Proof: all mantissas in `[1,4)`, normal exponent strides and edges.

![Error across the input range](../images/math/fast-path-mathf_rsqrt.png)

<a id="mathf-rsqrt-approx"></a>

## `mathf_rsqrt_approx`

```math
y = y(3/2 - (v/2)y^2)
```

Symbols: positive input `v`; reciprocal-root seed `y`.

Proof: all mantissas in `[1,4)`, normal exponent strides and edges.

![Error across the input range](../images/math/fast-path-mathf_rsqrt_approx.png)

<a id="mathx-rsqrt-q30"></a>

## `mathx_rsqrt_q30`

```math
y = round(2^{46}/sqrt(m))
```

Symbols: integer mantissa `m` in `[MATHX_RSQRT_MIN, 2^32)`; reciprocal root `y`.

Proof: mantissa segment strides and exact midpoint brackets.

![Error across the input range](../images/math/fast-path-mathx_rsqrt_q30.png)

<a id="mathf-repeat"></a>

## `mathf_repeat`

```math
r = fma(-floor(v/len), len, v)
```

Symbols: input `v` and positive period `len`; fused remainder `r`.

Sweep: signed float bit patterns, quotient guard and endpoint edges.

Bit-identical to `fmodf` over 97845 inputs.

<a id="trig-sin"></a>

## `trig_sin`

```math
s = a + floor((b-a)t/256)
```

Symbols: 16-bit phase input; quarter-wave samples `a` and `b` bracket it; `t` is the low eight phase bits and `s` is sine in output LSBs.

Sweep: every 16-bit phase against `sinf` in output LSBs.

![Error across the input range](../images/math/fast-path-trig_sin.png)
<!-- /generated: math-fast-paths -->
