# Math fast paths

The tables compare plain C with the package implementation: algorithm,
measured cost and error, linked size, build conditions, and calling modules.
The [capture commands](Benchmark.md#capture) produce the evidence.

Key: warm cycles measure cached batches; cold cycles include instruction-cache refill.
Pairs are fast vs plain; parentheses give plain / fast warm (greater than one wins).
Code totals wrappers and callees by ELF region; ? marks unknown ROM size. Tables total linked copies.
A dash means unmeasured. Package wrapper modules are included. App callers share the row “apps (not named)”.

<!-- generated: math-fast-paths check: python launcher/tools/perf/fast_paths.py --check -->
Measured target: -.

| Fast path | Plain way | Ours | Target / hardware feature | Warm cycles/op (ratio) | Cold cycles/call | Max error (limit; method) | Code bytes / region; table bytes (fast vs plain) |
|---|---|---|---|---|---|---|---|
| [`mathf_recip`](#mathf-recip) | `mathf_recip_f` / `div_recip_f`: `1.0F / x` | reciprocal seed with two Newton corrections | Target-specific; `madd.s`, `msub.s`, `recip0.s` | - vs - (-) | - vs - | - ulp (`MATHF_RECIP_ULP_LIMIT` (1); sweep). | 62 / flash; tables 0 vs 0 + ? / ROM, 45 / flash; tables 0 |
| [`mathf_rsqrt`](#mathf-rsqrt) | `mathf_rsqrt_f` / `div_rsqrt_f`: `1.0F / sqrtf(x)` | reciprocal-root seed with two Newton corrections | Target-specific; `madd.s`, `msub.s`, `rsqrt0.s` | - vs - (-) | - vs - | - ulp (`MATHF_RSQRT_ULP_LIMIT` (1); proof). | 0 + ? / ROM, 152 / flash; tables 0 vs 0 + ? / ROM, 65 / flash; tables 0 |
| [`mathf_rsqrt_approx`](#mathf-rsqrt-approx) | `mathf_rsqrt_approx_f` / `div_rsqrt_f`: `1.0F / sqrtf(x)` | reciprocal-root seed with one Newton correction | Target-specific; `msub.s`, `rsqrt0.s` | - vs - (-) | - vs - | - relative (`MATHF_RSQRT_APPROX_RELATIVE_LIMIT` (0.0001); proof). | 66 / flash; tables 0 vs 0 + ? / ROM, 65 / flash; tables 0 |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | `normalize_x` / `normalize_div_x`: `isqrt64 squared length, then 64-bit divide and component scaling`<br>`nlerp_x` / `nlerp_div_x`: `linear quaternion blend, isqrt64 length, then 64-bit divide normalization` | seed table, three Newton corrections and exact midpoint bracket | Portable C; no special instructions | `normalize_x`: - vs - (-)<br>`nlerp_x`: - vs - (-) | `normalize_x`: - vs -<br>`nlerp_x`: - vs - | - relative (`MATHX_RSQRT_Q30_RELATIVE_LIMIT` (4.65661e-10); proof). | `normalize_x`: 236 / IRAM, 956 / flash; tables 768 vs 582 / ROM, 2061 / flash; tables 0<br>`nlerp_x`: 236 / IRAM, 2664 / flash; tables 768 vs 606 / ROM, 3534 / flash; tables 0 |
| [`mathf_repeat`](#mathf-repeat) | `wrap_f` / `wrap_fmodf_f`: `fmodf(v - lo, hi - lo), adjusted into [lo, hi)` | reciprocal quotient and fused remainder, with range fallback | Portable C; `madd.s`, `msub.s`, `recip0.s` | - vs - (-) | - vs - | - ulp (`MATHF_WRAP_ULP_LIMIT` (0); sweep). | 612 / flash; tables 0 vs 460 / flash; tables 0 |
| [`trig_sin`](#trig-sin) | `trig_sin_i` / `sinf_f`: `sinf(phase * (MATH_TAU / TRIG_TURN))` | quarter-wave interpolation | Portable C; no special instructions | - vs - (-) | - vs - | - lsb (`TRIG_SIN_LSB_LIMIT` (5); sweep). | 202 / flash; tables 1040 vs 181 / flash; tables 0 |

### Builds and callers

| Fast path | Gating Kconfig | QEMU | Host | Engine modules |
|---|---|---|---|---|
| [`mathf_recip`](#mathf-recip) | `CONFIG_MATH_FPU_RECIP` | plain | plain | `math/linear`, `math/scalar`, `render` |
| [`mathf_rsqrt`](#mathf-rsqrt) | `CONFIG_MATH_FPU_RECIP` | plain | plain | `anim`, `apps (not named)`, `input`, `math`, `math/linear`, `scene` |
| [`mathf_rsqrt_approx`](#mathf-rsqrt-approx) | `CONFIG_MATH_FPU_RECIP` | plain | plain | `render` |
| [`mathx_rsqrt_q30`](#mathx-rsqrt-q30) | none | fast | fast | `math/linear`, `math/scalar` |
| [`mathf_repeat`](#mathf-repeat) | `CONFIG_MATH_FPU_FUSED_MADD` | plain | fast | `math/scalar` |
| [`trig_sin`](#trig-sin) | none | fast | fast | `audio`, `boot`, `math/scalar`, `shell` |

<a id="mathf-recip"></a>

## `mathf_recip`

```math
y = y + y(1 - ay)
```

Symbols: nonzero input `a`; reciprocal seed `y`.

Sweep: signed magnitudes and power-of-two edges.

Error curve: - (capture pending).

<a id="mathf-rsqrt"></a>

## `mathf_rsqrt`

```math
y = y + (y/2)(1 - vy^2)
```

Symbols: positive input `v`; reciprocal-root seed `y`.

Proof: all mantissas in `[1,4)`, normal exponent strides and edges.

Error curve: - (capture pending).

<a id="mathf-rsqrt-approx"></a>

## `mathf_rsqrt_approx`

```math
y = y(3/2 - (v/2)y^2)
```

Symbols: positive input `v`; reciprocal-root seed `y`.

Proof: all mantissas in `[1,4)`, normal exponent strides and edges.

Error curve: - (capture pending).

<a id="mathx-rsqrt-q30"></a>

## `mathx_rsqrt_q30`

```math
y = round(2^{46}/sqrt(m))
```

Symbols: integer mantissa `m` in `[MATHX_RSQRT_MIN, 2^32)`; reciprocal root `y`.

Proof: mantissa segment strides and exact midpoint brackets.

Error curve: - (capture pending).

<a id="mathf-repeat"></a>

## `mathf_repeat`

```math
r = fma(-floor(v/len), len, v)
```

Symbols: input `v` and positive period `len`; fused remainder `r`.

Sweep: signed float bit patterns, quotient guard and endpoint edges.

Error curve: - (capture pending).

<a id="trig-sin"></a>

## `trig_sin`

```math
s = a + floor((b-a)t/256)
```

Symbols: 16-bit phase input; quarter-wave samples `a` and `b` bracket it; `t` is the low eight phase bits and `s` is sine in output LSBs.

Sweep: every 16-bit phase against `sinf` in output LSBs.

Error curve: - (capture pending).
<!-- /generated: math-fast-paths -->
